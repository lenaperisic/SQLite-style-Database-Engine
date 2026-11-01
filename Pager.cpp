#include "Pager.h"
#include "ExceptionHandler.h"

#include <cstring>
#include <filesystem>

// Journal layout:
//   [ magic "MyDBjrnl": 8 ][ pages in the file before the commit: 4 ][ page size: 4 ]
//   then for every changed page: [ page number: 4 ][ old content: PAGE_SIZE ]
static const char JOURNAL_MAGIC[8] = {'M', 'y', 'D', 'B', 'j', 'r', 'n', 'l'};
static constexpr std::size_t JOURNAL_HEADER_SIZE = 16;

Pager::Pager(const std::string& filename)
    : journal_path(filename + "-journal") {
    recover_from_journal(filename);

    const auto mode = std::ios::in | std::ios::out | std::ios::binary;
    file.open(filename, mode);
    if (!file.is_open()) {
        // in|out mode doesn't create files, so create an empty one first
        std::ofstream(filename, std::ios::binary);
        file.open(filename, mode);
    }
    if (!file.is_open()) {
        throw DatabaseFileException("Unable to open file '" + filename + "'.");
    }

    file.seekg(0, std::ios::end);
    std::size_t file_length = static_cast<std::size_t>(file.tellg());
    if (file_length % PAGE_SIZE != 0) {
        throw DatabaseFileException("Db file is not a whole number of pages. Corrupt file.");
    }

    num_pages_on_disk = static_cast<std::uint32_t>(file_length / PAGE_SIZE);
    num_pages = num_pages_on_disk;
    pages.resize(num_pages);
}

void Pager::recover_from_journal(const std::string& filename) {
    std::error_code error;
    if (!std::filesystem::exists(journal_path, error)) {
        return;
    }

    std::ifstream journal(journal_path, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(journal)), std::istreambuf_iterator<char>());
    journal.close();

    // A journal cut off inside its header was being written when the program
    // died, before the database file was touched: nothing to undo
    bool valid = data.size() >= JOURNAL_HEADER_SIZE
                 && std::memcmp(data.data(), JOURNAL_MAGIC, sizeof(JOURNAL_MAGIC)) == 0;
    if (valid) {
        std::uint32_t old_num_pages;
        std::uint32_t page_size;
        std::memcpy(&old_num_pages, data.data() + 8, 4);
        std::memcpy(&page_size, data.data() + 12, 4);
        if (page_size != PAGE_SIZE) {
            throw DatabaseFileException("Journal has a different page size. Corrupt file.");
        }

        {
            std::fstream db(filename, std::ios::in | std::ios::out | std::ios::binary);
            if (!db.is_open()) {
                throw DatabaseFileException("Unable to open '" + filename + "' to roll back the journal.");
            }
            // Only whole entries; a cut-off last one was never followed by
            // writes to the database file
            std::size_t entry_size = 4 + PAGE_SIZE;
            for (std::size_t pos = JOURNAL_HEADER_SIZE; pos + entry_size <= data.size(); pos += entry_size) {
                std::uint32_t page_num;
                std::memcpy(&page_num, data.data() + pos, 4);
                db.seekp(static_cast<std::streamoff>(page_num) * PAGE_SIZE);
                db.write(data.data() + pos + 4, PAGE_SIZE);
            }
            db.flush();
            if (!db) {
                throw DatabaseFileException("Unable to roll back the journal.");
            }
        }

        // Pages added by the unfinished commit go away
        std::filesystem::resize_file(filename, static_cast<std::uintmax_t>(old_num_pages) * PAGE_SIZE, error);
        if (error) {
            throw DatabaseFileException("Unable to cut the file back after rolling back the journal.");
        }
    }

    std::filesystem::remove(journal_path, error);
    if (error) {
        throw DatabaseFileException("Unable to delete the journal '" + journal_path + "'.");
    }
}

char* Pager::get_page(std::uint32_t page_num) {
    if (page_num >= pages.size()) {
        pages.resize(page_num + 1);
    }

    if (!pages[page_num]) {
        // Cache miss: new zeroed page, filled from disk if it exists there
        pages[page_num] = std::make_unique<char[]>(PAGE_SIZE);

        if (page_num < num_pages_on_disk) {
            file.clear();
            file.seekg(static_cast<std::streamoff>(page_num) * PAGE_SIZE);
            file.read(pages[page_num].get(), PAGE_SIZE);
            if (!file) {
                throw DatabaseFileException("Unable to read page "
                                            + std::to_string(page_num) + ".");
            }
        }
    }

    if (page_num >= num_pages) {
        num_pages = page_num + 1;
    }

    // First use in this transaction: remember the committed content.
    // Pages added during the transaction have nothing to remember.
    if (transaction_active && page_num < pages_at_begin && originals.count(page_num) == 0) {
        auto copy = std::make_unique<char[]>(PAGE_SIZE);
        std::memcpy(copy.get(), pages[page_num].get(), PAGE_SIZE);
        originals[page_num] = std::move(copy);
    }

    return pages[page_num].get();
}

std::uint32_t Pager::get_num_pages() const {
    return num_pages;
}

std::uint32_t Pager::get_unused_page_num() const {
    return num_pages;
}

void Pager::begin() {
    transaction_active = true;
    pages_at_begin = num_pages;
    originals.clear();
}

void Pager::commit() {
    // Pages whose content changed, plus every page added in the transaction
    std::vector<std::uint32_t> changed;
    for (const auto& entry : originals) {
        if (std::memcmp(entry.second.get(), pages[entry.first].get(), PAGE_SIZE) != 0) {
            changed.push_back(entry.first);
        }
    }
    for (std::uint32_t i = pages_at_begin; i < num_pages; i++) {
        get_page(i);    // a page number can be skipped, make sure it exists
        changed.push_back(i);
    }

    if (!changed.empty()) {
        write_journal(changed);

        for (std::uint32_t page_num : changed) {
            write_page(page_num);
        }
        file.flush();
        if (!file) {
            throw DatabaseFileException("Unable to write the database file.");
        }

        // Deleting the journal is the commit: from now on the new pages count
        std::error_code error;
        std::filesystem::remove(journal_path, error);
        if (error) {
            throw DatabaseFileException("Unable to delete the journal '" + journal_path + "'.");
        }
        num_pages_on_disk = num_pages;
    }

    originals.clear();
    transaction_active = false;
}

void Pager::rollback() {
    for (auto& entry : originals) {
        std::memcpy(pages[entry.first].get(), entry.second.get(), PAGE_SIZE);
    }
    originals.clear();

    // Forget pages added since begin()
    if (pages.size() > pages_at_begin) {
        pages.resize(pages_at_begin);
    }
    num_pages = pages_at_begin;
    transaction_active = false;
}

void Pager::write_journal(const std::vector<std::uint32_t>& changed) {
    std::ofstream journal(journal_path, std::ios::binary | std::ios::trunc);
    if (!journal.is_open()) {
        throw DatabaseFileException("Unable to create the journal '" + journal_path + "'.");
    }

    std::uint32_t page_size = PAGE_SIZE;
    journal.write(JOURNAL_MAGIC, sizeof(JOURNAL_MAGIC));
    journal.write(reinterpret_cast<const char*>(&pages_at_begin), 4);
    journal.write(reinterpret_cast<const char*>(&page_size), 4);

    // New pages have no old content, cutting the file back removes them
    for (std::uint32_t page_num : changed) {
        if (page_num < pages_at_begin) {
            journal.write(reinterpret_cast<const char*>(&page_num), 4);
            journal.write(originals[page_num].get(), PAGE_SIZE);
        }
    }

    journal.flush();
    if (!journal) {
        throw DatabaseFileException("Unable to write the journal '" + journal_path + "'.");
    }
}

void Pager::write_page(std::uint32_t page_num) {
    file.clear();
    file.seekp(static_cast<std::streamoff>(page_num) * PAGE_SIZE);
    file.write(pages[page_num].get(), PAGE_SIZE);
    if (!file) {
        throw DatabaseFileException("Unable to write page " + std::to_string(page_num) + ".");
    }
}
