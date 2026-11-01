#include "Database.h"
#include "Cursor.h"
#include "ExceptionHandler.h"
#include "Node.h"
#include "Parser.h"

#include <cctype>
#include <cstring>

static const char MAGIC[8] = {'M', 'y', 'D', 'B', ' ', 'v', '1', '\0'};
static constexpr std::size_t MAGIC_OFFSET = 0;
static constexpr std::size_t PAGE_SIZE_OFFSET = 8;
static constexpr std::size_t FIRST_FREE_OFFSET = 12;

// Columns of the schema table
static constexpr std::size_t SCHEMA_TYPE = 0;
static constexpr std::size_t SCHEMA_NAME = 1;
static constexpr std::size_t SCHEMA_ROOT_PAGE = 3;
static constexpr std::size_t SCHEMA_SQL = 4;

static std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

static std::uint32_t read_u32(const char* data) {
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

static void write_u32(char* data, std::uint32_t value) {
    std::memcpy(data, &value, sizeof(value));
}

Database::Database(const std::string& filename)
    : pager(filename) {
    if (pager.get_num_pages() == 0) {
        pager.begin();
        initialize_file();
        pager.commit();
    } else {
        check_header();
    }
    load_schema();
}

Database::~Database() {
    // Everything committed is already on disk; an open transaction is dropped
    if (pager.in_transaction()) {
        pager.rollback();
    }
}

void Database::begin() {
    if (pager.in_transaction()) {
        throw TransactionException("cannot start a transaction within a transaction");
    }
    pager.begin();
}

void Database::commit() {
    if (!pager.in_transaction()) {
        throw TransactionException("cannot commit - no transaction is active");
    }
    pager.commit();
}

void Database::rollback() {
    if (!pager.in_transaction()) {
        throw TransactionException("cannot rollback - no transaction is active");
    }
    pager.rollback();


    tables.clear();
    load_schema();
}

bool Database::in_transaction() const {
    return pager.in_transaction();
}

void Database::initialize_file() {
    char* header = pager.get_page(HEADER_PAGE_NUM);
    std::memcpy(header + MAGIC_OFFSET, MAGIC, sizeof(MAGIC));
    write_u32(header + PAGE_SIZE_OFFSET, Pager::PAGE_SIZE);
    write_u32(header + FIRST_FREE_OFFSET, 0);

    Table::initialize_root(pager, SCHEMA_ROOT_PAGE_NUM);
}

void Database::check_header() {
    const char* header = pager.get_page(HEADER_PAGE_NUM);
    if (std::memcmp(header + MAGIC_OFFSET, MAGIC, sizeof(MAGIC)) != 0) {
        throw DatabaseFileException("File is not a database (or an old format). Delete it and start again.");
    }
    if (read_u32(header + PAGE_SIZE_OFFSET) != Pager::PAGE_SIZE) {
        throw DatabaseFileException("Database uses a different page size.");
    }
    if (pager.get_num_pages() < 2) {
        throw DatabaseFileException("Database has no schema page. Corrupt file.");
    }
}

void Database::load_schema() {
    Table schema_table(*this, SCHEMA_ROOT_PAGE_NUM);

    for (Cursor cursor = Cursor::table_start(schema_table); !cursor.is_end(); cursor.advance()) {
        Row row = Row::deserialize(cursor.get_key(), cursor.value());
        const std::vector<Value>& values = row.get_values();
        if (values.size() != 5 || values[SCHEMA_SQL].get_type() != DataType::Text
            || values[SCHEMA_ROOT_PAGE].get_type() != DataType::Integer) {
            throw DatabaseFileException("Bad row in the schema table. Corrupt file.");
        }

        // The schema keeps the CREATE TABLE text, parse it to get the columns back
        Statement statement = Parser(values[SCHEMA_SQL].as_text()).parse();
        TableSchema schema = statement.schema;
        schema.root_page = static_cast<std::uint32_t>(values[SCHEMA_ROOT_PAGE].as_integer());
        tables[to_lower(schema.name)] = schema;
    }
}

void Database::create_table(TableSchema schema) {
    std::string key = to_lower(schema.name);
    if (tables.count(key) != 0) {
        throw TableExistsException(schema.name);
    }

    schema.root_page = allocate_page();
    Table::initialize_root(pager, schema.root_page);

    Table schema_table(*this, SCHEMA_ROOT_PAGE_NUM);
    std::uint32_t schema_id = schema_table.is_empty() ? 1 : schema_table.get_max_key() + 1;
    try {
        schema_table.insert(Row(schema_id, {
            Value::text("table"),
            Value::text(schema.name),
            Value::text(schema.name),
            Value::integer(schema.root_page),
            Value::text(schema.sql)
        }));
    }
    catch (const ExceptionHandler&) {
        // e.g. the CREATE TABLE text is too long for a row: give the page back
        free_page(schema.root_page);
        throw;
    }

    tables[key] = schema;
}

void Database::drop_table(const std::string& name) {
    const TableSchema& schema = get_schema(name);

    // Find the table's row in the schema table
    Table schema_table(*this, SCHEMA_ROOT_PAGE_NUM);
    std::uint32_t schema_id = 0;
    bool found = false;
    for (Cursor cursor = Cursor::table_start(schema_table); !cursor.is_end(); cursor.advance()) {
        Row row = Row::deserialize(cursor.get_key(), cursor.value());
        if (to_lower(row.get_values()[SCHEMA_NAME].as_text()) == to_lower(name)) {
            schema_id = cursor.get_key();
            found = true;
            break;
        }
    }
    if (!found) {
        throw DatabaseFileException("Table " + name + " is missing from the schema table. Corrupt file.");
    }

    Table(*this, schema.root_page).drop();
    schema_table.remove(schema_id);
    tables.erase(to_lower(name));
}

const TableSchema& Database::get_schema(const std::string& name) const {
    auto it = tables.find(to_lower(name));
    if (it == tables.end()) {
        throw TableNotFoundException(name);
    }
    return it->second;
}

std::vector<const TableSchema*> Database::get_schemas() const {
    std::vector<const TableSchema*> result;
    for (const auto& entry : tables) {
        result.push_back(&entry.second);
    }
    return result;
}

Table Database::get_table(const std::string& name) {
    return Table(*this, get_schema(name).root_page);
}

std::uint32_t Database::allocate_page() {
    std::uint32_t page_num = get_first_free();
    if (page_num == 0) {
        return pager.get_unused_page_num();
    }

    set_first_free(Node(pager.get_page(page_num)).get_next_free());
    return page_num;
}

void Database::free_page(std::uint32_t page_num) {
    Node page(pager.get_page(page_num));
    page.set_type(NodeType::Free);
    page.set_root(false);
    page.set_next_free(get_first_free());
    set_first_free(page_num);
}

std::uint32_t Database::get_first_free() {
    return read_u32(pager.get_page(HEADER_PAGE_NUM) + FIRST_FREE_OFFSET);
}

void Database::set_first_free(std::uint32_t page_num) {
    write_u32(pager.get_page(HEADER_PAGE_NUM) + FIRST_FREE_OFFSET, page_num);
}
