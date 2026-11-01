#include "Node.h"
#include "ExceptionHandler.h"

#include <cstring>
#include <string>

NodeType Node::get_type() const {
    return static_cast<NodeType>(data[NODE_TYPE_OFFSET]);
}

void Node::set_type(NodeType type) {
    data[NODE_TYPE_OFFSET] = static_cast<char>(type);
}

bool Node::is_root() const {
    return data[IS_ROOT_OFFSET] != 0;
}

void Node::set_root(bool root) {
    data[IS_ROOT_OFFSET] = root ? 1 : 0;
}

std::uint32_t Node::get_parent() const {
    return read_u32(PARENT_OFFSET);
}

void Node::set_parent(std::uint32_t page_num) {
    write_u32(PARENT_OFFSET, page_num);
}

std::uint32_t Node::get_next_free() const {
    return read_u32(NEXT_FREE_OFFSET);
}

void Node::set_next_free(std::uint32_t page_num) {
    write_u32(NEXT_FREE_OFFSET, page_num);
}

std::uint32_t Node::read_u32(std::size_t offset) const {
    std::uint32_t value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

void Node::write_u32(std::size_t offset, std::uint32_t value) {
    std::memcpy(data + offset, &value, sizeof(value));
}

// ---------- LeafNode ----------

void LeafNode::initialize() {
    set_type(NodeType::Leaf);
    set_root(false);
    set_parent(0);
    set_num_cells(0);
    set_next_leaf(0);
}

std::uint32_t LeafNode::get_num_cells() const {
    return read_u32(NUM_CELLS_OFFSET);
}

void LeafNode::set_num_cells(std::uint32_t num_cells) {
    write_u32(NUM_CELLS_OFFSET, num_cells);
}

std::uint32_t LeafNode::get_next_leaf() const {
    return read_u32(NEXT_LEAF_OFFSET);
}

void LeafNode::set_next_leaf(std::uint32_t page_num) {
    write_u32(NEXT_LEAF_OFFSET, page_num);
}

void LeafNode::remove_cell(std::uint32_t cell_num) {
    std::uint32_t num_cells = get_num_cells();
    if (cell_num + 1 < num_cells) {
        std::memmove(cell(cell_num), cell(cell_num + 1),
                     (num_cells - cell_num - 1) * CELL_SIZE);
    }
    set_num_cells(num_cells - 1);
}

char* LeafNode::cell(std::uint32_t cell_num) {
    return data + HEADER_SIZE + cell_num * CELL_SIZE;
}

std::uint32_t LeafNode::get_key(std::uint32_t cell_num) const {
    return read_u32(HEADER_SIZE + cell_num * CELL_SIZE);
}

void LeafNode::set_key(std::uint32_t cell_num, std::uint32_t key) {
    write_u32(HEADER_SIZE + cell_num * CELL_SIZE, key);
}

char* LeafNode::value(std::uint32_t cell_num) {
    return cell(cell_num) + KEY_SIZE;
}

std::uint32_t LeafNode::find_cell(std::uint32_t key) const {
    std::uint32_t low = 0;
    std::uint32_t high = get_num_cells();
    while (low < high) {
        std::uint32_t mid = low + (high - low) / 2;
        if (get_key(mid) >= key) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

// ---------- InternalNode ----------

void InternalNode::initialize() {
    set_type(NodeType::Internal);
    set_root(false);
    set_parent(0);
    set_num_keys(0);
    set_right_child(0);
}

void InternalNode::remove_child(std::uint32_t child_num) {
    std::uint32_t num_keys = get_num_keys();

    if (child_num == num_keys) {
        // The last cell's child becomes the right child, its key is dropped
        set_right_child(get_child(num_keys - 1));
    } else {
        for (std::uint32_t i = child_num; i + 1 < num_keys; i++) {
            set_child(i, get_child(i + 1));
            set_key(i, get_key(i + 1));
        }
    }

    set_num_keys(num_keys - 1);
}

std::uint32_t InternalNode::get_num_keys() const {
    return read_u32(NUM_KEYS_OFFSET);
}

void InternalNode::set_num_keys(std::uint32_t num_keys) {
    write_u32(NUM_KEYS_OFFSET, num_keys);
}

std::uint32_t InternalNode::get_right_child() const {
    return read_u32(RIGHT_CHILD_OFFSET);
}

void InternalNode::set_right_child(std::uint32_t page_num) {
    write_u32(RIGHT_CHILD_OFFSET, page_num);
}

std::size_t InternalNode::cell_offset(std::uint32_t cell_num) const {
    return HEADER_SIZE + cell_num * CELL_SIZE;
}

std::uint32_t InternalNode::get_child(std::uint32_t child_num) const {
    std::uint32_t num_keys = get_num_keys();
    if (child_num > num_keys) {
        throw DatabaseFileException("Tried to access child " + std::to_string(child_num)
                                    + " of a node with " + std::to_string(num_keys) + " keys.");
    }
    if (child_num == num_keys) {
        return get_right_child();
    }
    return read_u32(cell_offset(child_num));
}

void InternalNode::set_child(std::uint32_t child_num, std::uint32_t page_num) {
    if (child_num == get_num_keys()) {
        set_right_child(page_num);
    } else {
        write_u32(cell_offset(child_num), page_num);
    }
}

std::uint32_t InternalNode::get_key(std::uint32_t key_num) const {
    return read_u32(cell_offset(key_num) + CHILD_SIZE);
}

void InternalNode::set_key(std::uint32_t key_num, std::uint32_t key) {
    write_u32(cell_offset(key_num) + CHILD_SIZE, key);
}

std::uint32_t InternalNode::find_child(std::uint32_t key) const {
    std::uint32_t low = 0;
    std::uint32_t high = get_num_keys();
    while (low < high) {
        std::uint32_t mid = low + (high - low) / 2;
        if (get_key(mid) >= key) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

std::uint32_t InternalNode::index_of_child(std::uint32_t page_num) const {
    std::uint32_t num_keys = get_num_keys();
    for (std::uint32_t i = 0; i <= num_keys; i++) {
        if (get_child(i) == page_num) {
            return i;
        }
    }
    throw DatabaseFileException("Page " + std::to_string(page_num)
                                + " is not a child of its parent. Corrupt file.");
}
