#ifndef DATABASE_H
#define DATABASE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Pager.h"
#include "Schema.h"
#include "Table.h"

// One database file with any number of tables.
//
// Page 0 is the file header:
//   [ magic "MyDB v1\0": 8 ][ page size: 4 ][ first free page: 4 ]
// Page 1 is the root of the schema table, a normal B+ tree with one row per
// table:  (type 'table', name, table name, root page, CREATE TABLE text)
// Every other page belongs to some table's tree or to the free list, in any
// order: a table that needs a page takes the first free one or a new one at
// the end of the file.
class Database {
public:
    static constexpr std::uint32_t HEADER_PAGE_NUM = 0;
    static constexpr std::uint32_t SCHEMA_ROOT_PAGE_NUM = 1;

    // Opens the file, creating an empty database if it's new.
    // Throws DatabaseFileException if it isn't one of our database files.
    explicit Database(const std::string& filename);
    // Drops an open transaction (committed changes are already on disk)
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Throws TableExistsException
    void create_table(TableSchema schema);
    // Throws TableNotFoundException
    void drop_table(const std::string& name);

    // Throws TableNotFoundException
    const TableSchema& get_schema(const std::string& name) const;
    // All tables, sorted by name
    std::vector<const TableSchema*> get_schemas() const;
    // The B+ tree of a table (throws TableNotFoundException)
    Table get_table(const std::string& name);

    Pager& get_pager() { return pager; }

    // Transactions (see Pager). Each throws TransactionException when used
    // in the wrong state, e.g. commit() with no transaction.
    void begin();
    void commit();
    // Also reloads the tables, in case CREATE / DROP TABLE was undone
    void rollback();
    bool in_transaction() const;

    // Takes a page from the free list, or a new page at the end of the file
    std::uint32_t allocate_page();
    // Puts a page no longer used by any tree on the free list
    void free_page(std::uint32_t page_num);

private:
    void initialize_file();
    void check_header();
    // Reads the schema table and parses every CREATE TABLE again
    void load_schema();

    std::uint32_t get_first_free();
    void set_first_free(std::uint32_t page_num);

    Pager pager;
    // Table names are case-insensitive, so the key is the lowercase name
    std::map<std::string, TableSchema> tables;
};

#endif //DATABASE_H
