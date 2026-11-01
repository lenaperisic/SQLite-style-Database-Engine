#ifndef TABLE_H
#define TABLE_H

#include <cstdint>

#include "Pager.h"
#include "Row.h"

class Database;

// One table's rows, stored in a B+ tree keyed by row id. The root stays on
// the same page for the whole life of the table (the schema remembers it).
// Leaves hold the rows sorted by id and are linked left to right,
// internal nodes only hold keys and child page numbers.
//
// Table doesn't own anything: pages belong to the Database's pager, so a
// Table is cheap to create whenever a statement needs one.
class Table {
public:
    Table(Database& database, std::uint32_t root_page_num);

    // Turns a fresh page into an empty tree (a root leaf with no cells)
    static void initialize_root(Pager& pager, std::uint32_t page_num);

    // Throws DuplicateKeyException if a row with this id already exists
    void insert(const Row& row);
    // Throws RowNotFoundException if there's no row with this id
    void remove(std::uint32_t key);
    // Replaces the values of the row with the same id, in place.
    // Throws RowNotFoundException if there's no such row.
    void update(const Row& row);
    // Removes every row, keeps the (now empty) root
    void clear();
    // Frees every page of the tree, including the root
    void drop();

    bool is_empty();
    // Largest row id in the table (0 if empty)
    std::uint32_t get_max_key();
    std::uint32_t get_root_page_num() const { return root_page_num; }

private:
    // Cursor walks the tree through the pager
    friend class Cursor;

    void leaf_insert(std::uint32_t page_num, std::uint32_t cell_num, std::uint32_t key, const Row& row);
    void leaf_split_and_insert(std::uint32_t page_num, std::uint32_t cell_num, std::uint32_t key, const Row& row);

    // Adds child_page to the internal node parent_page after a split
    void internal_insert(std::uint32_t parent_page_num, std::uint32_t child_page_num);
    void internal_split_and_insert(std::uint32_t page_num, std::uint32_t child_page_num);

    // Root was split into the current root and right_child_page_num.
    // Moves the old root's content to a new page so the root keeps its page.
    void create_new_root(std::uint32_t right_child_page_num);

    // After a node shrank in a split, fixes the key its parent keeps for it
    void update_key_in_parent(std::uint32_t page_num);

    // Deleting
    void leaf_delete(std::uint32_t page_num, std::uint32_t cell_num);
    // A non-root leaf has too few cells: borrow from a sibling or merge
    void rebalance_leaf(std::uint32_t page_num);
    // A non-root internal node has too few children: borrow or merge
    void rebalance_internal(std::uint32_t page_num);
    // A child of parent_page_num was removed by a merge
    void after_child_removed(std::uint32_t parent_page_num);
    // Root is an internal node with one child: that child becomes the root
    void collapse_root();

    // A node's largest key changed: fix the key stored for it in its parent,
    // and keep going up while the node is its parent's right child
    // (the right child's max is also the parent's max)
    void refresh_max_key(std::uint32_t page_num);

    // Largest key in the subtree rooted at page_num
    std::uint32_t get_max_key(std::uint32_t page_num);

    // Frees the children of an internal node, recursively
    void free_children(std::uint32_t page_num);

    Database& database;
    Pager& pager;
    std::uint32_t root_page_num;
};

#endif //TABLE_H
