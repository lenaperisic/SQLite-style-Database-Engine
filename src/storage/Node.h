#ifndef NODE_H
#define NODE_H

#include <cstddef>
#include <cstdint>

#include "Pager.h"
#include "Row.h"

// Every page of the file except page 0 (the file header) is a node of some
// table's B+ tree, or a free page. These classes don't own any memory, they
// just read and write fields inside a page.
//
// Common header (6 bytes):
//   [ node type: 1 ][ is root: 1 ][ parent page: 4 ]
//   A free page keeps the next free page where other nodes keep their parent
//   (0 means it's the last one).
//
// Leaf node:
//   [ common header ][ num cells: 4 ][ next leaf: 4 ][ cell 0 ][ cell 1 ]...
//   cell = [ key: 4 ][ row: 293 ]
//
// Internal node:
//   [ common header ][ num keys: 4 ][ right child: 4 ][ cell 0 ][ cell 1 ]...
//   cell = [ child page: 4 ][ key: 4 ]
//   key i is the largest key in child i's subtree; the right child holds
//   everything bigger than the last key.

enum class NodeType : std::uint8_t {
    Internal = 0,
    Leaf = 1,
    Free = 2    // page no longer used by any tree, waiting to be reused
};

class Node {
public:
    static constexpr std::size_t NODE_TYPE_OFFSET = 0;
    static constexpr std::size_t IS_ROOT_OFFSET = 1;
    static constexpr std::size_t PARENT_OFFSET = 2;
    static constexpr std::size_t NEXT_FREE_OFFSET = PARENT_OFFSET;
    static constexpr std::size_t COMMON_HEADER_SIZE = 6;

    explicit Node(char* data) : data(data) {}

    NodeType get_type() const;
    void set_type(NodeType type);
    bool is_root() const;
    void set_root(bool root);
    std::uint32_t get_parent() const;
    void set_parent(std::uint32_t page_num);
    // Only for free pages
    std::uint32_t get_next_free() const;
    void set_next_free(std::uint32_t page_num);

    char* get_data() const { return data; }

protected:
    // memcpy instead of casting, fields aren't aligned inside the page
    std::uint32_t read_u32(std::size_t offset) const;
    void write_u32(std::size_t offset, std::uint32_t value);

    char* data;
};

class LeafNode : public Node {
public:
    static constexpr std::size_t NUM_CELLS_OFFSET = COMMON_HEADER_SIZE;
    static constexpr std::size_t NEXT_LEAF_OFFSET = NUM_CELLS_OFFSET + 4;
    static constexpr std::size_t HEADER_SIZE = NEXT_LEAF_OFFSET + 4;

    static constexpr std::size_t KEY_SIZE = 4;
    static constexpr std::size_t CELL_SIZE = KEY_SIZE + Row::ROW_SIZE;
    static constexpr std::size_t MAX_CELLS = (Pager::PAGE_SIZE - HEADER_SIZE) / CELL_SIZE;
    // When a full leaf gets one more cell, the MAX_CELLS + 1 cells are split
    static constexpr std::size_t RIGHT_SPLIT_COUNT = (MAX_CELLS + 1) / 2;
    static constexpr std::size_t LEFT_SPLIT_COUNT = (MAX_CELLS + 1) - RIGHT_SPLIT_COUNT;
    // A non-root leaf with fewer cells borrows from or merges with a sibling
    static constexpr std::size_t MIN_CELLS = MAX_CELLS / 2;

    explicit LeafNode(char* data) : Node(data) {}

    // Empty, non-root leaf with no next leaf
    void initialize();

    // Removes a cell, shifting the cells after it left
    void remove_cell(std::uint32_t cell_num);

    std::uint32_t get_num_cells() const;
    void set_num_cells(std::uint32_t num_cells);
    // 0 means this is the last leaf (page 0 is always the root, never a sibling)
    std::uint32_t get_next_leaf() const;
    void set_next_leaf(std::uint32_t page_num);

    char* cell(std::uint32_t cell_num);
    std::uint32_t get_key(std::uint32_t cell_num) const;
    void set_key(std::uint32_t cell_num, std::uint32_t key);
    char* value(std::uint32_t cell_num);

    // Binary search: index of the first cell whose key is >= key
    // (num_cells if every key is smaller)
    std::uint32_t find_cell(std::uint32_t key) const;
};

class InternalNode : public Node {
public:
    static constexpr std::size_t NUM_KEYS_OFFSET = COMMON_HEADER_SIZE;
    static constexpr std::size_t RIGHT_CHILD_OFFSET = NUM_KEYS_OFFSET + 4;
    static constexpr std::size_t HEADER_SIZE = RIGHT_CHILD_OFFSET + 4;

    static constexpr std::size_t CHILD_SIZE = 4;
    static constexpr std::size_t KEY_SIZE = 4;
    static constexpr std::size_t CELL_SIZE = CHILD_SIZE + KEY_SIZE;
    static constexpr std::size_t MAX_KEYS = (Pager::PAGE_SIZE - HEADER_SIZE) / CELL_SIZE;
    // Children = keys + 1 (the right child). A non-root internal node with
    // fewer children borrows from or merges with a sibling.
    static constexpr std::size_t MAX_CHILDREN = MAX_KEYS + 1;
    static constexpr std::size_t MIN_CHILDREN = MAX_CHILDREN / 2;

    explicit InternalNode(char* data) : Node(data) {}

    // Empty, non-root internal node
    void initialize();

    std::uint32_t get_num_children() const { return get_num_keys() + 1; }

    // Removes a child and its key. Removing the right child makes the child
    // before it the new right child.
    void remove_child(std::uint32_t child_num);

    std::uint32_t get_num_keys() const;
    void set_num_keys(std::uint32_t num_keys);
    std::uint32_t get_right_child() const;
    void set_right_child(std::uint32_t page_num);

    // child_num == num_keys means the right child
    std::uint32_t get_child(std::uint32_t child_num) const;
    void set_child(std::uint32_t child_num, std::uint32_t page_num);
    std::uint32_t get_key(std::uint32_t key_num) const;
    void set_key(std::uint32_t key_num, std::uint32_t key);

    // Binary search: which child the key belongs to (num_keys = right child)
    std::uint32_t find_child(std::uint32_t key) const;
    // Position of a child page among the children (num_keys = right child)
    std::uint32_t index_of_child(std::uint32_t page_num) const;

private:
    std::size_t cell_offset(std::uint32_t cell_num) const;
};

#endif //NODE_H
