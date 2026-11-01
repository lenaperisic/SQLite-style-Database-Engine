#ifndef CURSOR_H
#define CURSOR_H

#include <cstdint>

class Table;

// Points at one cell of a leaf in the table's B+ tree. Code that reads or
// writes rows goes through a cursor, so it doesn't need to know about nodes.
class Cursor {
public:
    // Cursor at the row with the smallest id
    static Cursor table_start(Table& table);
    // Cursor at the row with this id, or where it would be inserted
    static Cursor table_find(Table& table, std::uint32_t key);

    // Address of the current row's bytes inside its leaf
    char* value();
    // Key (row id) of the current row
    std::uint32_t get_key();
    // True if the cursor points at an existing row with this key
    // (table_find stops where the key would go, even if it isn't there)
    bool is_at(std::uint32_t key);
    // Moves to the next row, following the link to the next leaf
    void advance();
    // True when the cursor is past the last row
    bool is_end() const;

    std::uint32_t get_page_num() const;
    std::uint32_t get_cell_num() const;

private:
    Cursor(Table& table, std::uint32_t page_num, std::uint32_t cell_num);

    Table& table;
    std::uint32_t page_num;
    std::uint32_t cell_num;
    bool end_of_table;
};

#endif //CURSOR_H
