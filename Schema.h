#ifndef SCHEMA_H
#define SCHEMA_H

#include <cstdint>
#include <string>
#include <vector>

#include "Value.h"

// One column from CREATE TABLE
struct Column {
    std::string name;
    DataType type;              // the column's affinity, from the declared type
    bool primary_key = false;
};

// Everything the database knows about one table. It's saved in the schema
// table as the original CREATE TABLE text and rebuilt by parsing it again
// when the database is opened.
struct TableSchema {
    std::string name;
    std::vector<Column> columns;
    std::uint32_t root_page = 0;
    std::string sql;

    // Index of the INTEGER PRIMARY KEY column, or -1 if there is none.
    // That column is the row id: its value is the key in the B+ tree
    // and the record stores NULL in its place.
    int primary_key_index() const {
        for (std::size_t i = 0; i < columns.size(); i++) {
            if (columns[i].primary_key) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

#endif //SCHEMA_H
