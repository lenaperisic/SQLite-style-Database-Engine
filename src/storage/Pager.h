#ifndef PAGER_H
#define PAGER_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Keeps pages of the database file in memory and writes them back only when
// a transaction commits, so the file always holds the last committed state.
//
// Changing pages:
//   begin()     start remembering pages: the first time a page is used, a
//               copy of its committed content is kept
//   commit()    1. write the "<file>-journal": the old content of every page
//                  that changed, and how many pages the file had
//               2. write the changed pages into the database file
//               3. delete the journal (this is the moment of commit)
//   rollback()  put the kept copies back, forget pages added since begin()
//
// If the program dies during step 2, the file is half written, but the
// journal is still there: the next time the file is opened, the old pages
// are copied back from it and the file is cut to its old size. If it dies
// before step 2, the file was never touched.
//
// The journal and the database are flushed to the operating system, but not
// forced to the physical disk (no fsync), so this protects against the
// program crashing, not against the computer losing power.
class Pager {
public:
    static constexpr std::size_t PAGE_SIZE = 4096;

    // Opens the file, creating it if it doesn't exist. Rolls back a commit
    // that didn't finish if a journal was left behind.
    // Throws DatabaseFileException if it can't be opened or isn't whole pages.
    explicit Pager(const std::string& filename);

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    // Returns the page in memory, loading it from disk on first use.
    // Asking for a page past the end makes the file one page longer.
    char* get_page(std::uint32_t page_num);

    // Pages in the file, counting new ones not yet written
    std::uint32_t get_num_pages() const;
    // New pages always go at the end of the file
    std::uint32_t get_unused_page_num() const;

    void begin();
    // Throws DatabaseFileException if writing fails (the journal stays, so
    // the next open restores the old state)
    void commit();
    void rollback();
    bool in_transaction() const { return transaction_active; }

    std::string get_journal_path() const { return journal_path; }

private:
    // Copies a journal's old pages back into the file, then deletes it
    void recover_from_journal(const std::string& filename);
    void write_journal(const std::vector<std::uint32_t>& changed);
    void write_page(std::uint32_t page_num);

    std::fstream file;
    std::string journal_path;
    std::uint32_t num_pages_on_disk;
    std::uint32_t num_pages;
    std::vector<std::unique_ptr<char[]>> pages;

    bool transaction_active = false;
    std::uint32_t pages_at_begin = 0;
    // Committed content of every page used in the current transaction
    std::map<std::uint32_t, std::unique_ptr<char[]>> originals;
};

#endif //PAGER_H
