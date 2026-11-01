#include "Table.h"
#include "Cursor.h"
#include "Database.h"
#include "ExceptionHandler.h"
#include "Node.h"

#include <cstring>
#include <vector>

Table::Table(Database& database, std::uint32_t root_page_num)
    : database(database), pager(database.get_pager()), root_page_num(root_page_num) {}

void Table::initialize_root(Pager& pager, std::uint32_t page_num) {
    LeafNode root(pager.get_page(page_num));
    root.initialize();
    root.set_root(true);
}

void Table::insert(const Row& row) {
    std::uint32_t key = row.get_id();
    Cursor cursor = Cursor::table_find(*this, key);

    LeafNode leaf(pager.get_page(cursor.get_page_num()));
    if (cursor.get_cell_num() < leaf.get_num_cells()
        && leaf.get_key(cursor.get_cell_num()) == key) {
        throw DuplicateKeyException();
    }

    leaf_insert(cursor.get_page_num(), cursor.get_cell_num(), key, row);
}

void Table::remove(std::uint32_t key) {
    Cursor cursor = Cursor::table_find(*this, key);

    LeafNode leaf(pager.get_page(cursor.get_page_num()));
    if (cursor.get_cell_num() >= leaf.get_num_cells()
        || leaf.get_key(cursor.get_cell_num()) != key) {
        throw RowNotFoundException(key);
    }

    leaf_delete(cursor.get_page_num(), cursor.get_cell_num());
}

void Table::update(const Row& row) {
    std::uint32_t key = row.get_id();
    Cursor cursor = Cursor::table_find(*this, key);

    LeafNode leaf(pager.get_page(cursor.get_page_num()));
    if (cursor.get_cell_num() >= leaf.get_num_cells()
        || leaf.get_key(cursor.get_cell_num()) != key) {
        throw RowNotFoundException(key);
    }

    row.serialize(leaf.value(cursor.get_cell_num()));
}

void Table::leaf_insert(std::uint32_t page_num, std::uint32_t cell_num, std::uint32_t key, const Row& row) {
    LeafNode leaf(pager.get_page(page_num));
    std::uint32_t num_cells = leaf.get_num_cells();

    if (num_cells >= LeafNode::MAX_CELLS) {
        leaf_split_and_insert(page_num, cell_num, key, row);
        return;
    }

    // Shift the cells after cell_num one place right to make room
    if (cell_num < num_cells) {
        std::memmove(leaf.cell(cell_num + 1), leaf.cell(cell_num),
                     (num_cells - cell_num) * LeafNode::CELL_SIZE);
    }

    leaf.set_key(cell_num, key);
    row.serialize(leaf.value(cell_num));
    leaf.set_num_cells(num_cells + 1);
}

void Table::leaf_split_and_insert(std::uint32_t page_num, std::uint32_t cell_num, std::uint32_t key, const Row& row) {
    // Lay out all MAX_CELLS + 1 cells in order, then give the first half to
    // the old leaf and the second half to a new leaf on its right
    std::vector<char> cells((LeafNode::MAX_CELLS + 1) * LeafNode::CELL_SIZE);
    {
        LeafNode old_leaf(pager.get_page(page_num));
        std::memcpy(cells.data(), old_leaf.cell(0), cell_num * LeafNode::CELL_SIZE);

        char* new_cell = cells.data() + cell_num * LeafNode::CELL_SIZE;
        std::memcpy(new_cell, &key, LeafNode::KEY_SIZE);
        row.serialize(new_cell + LeafNode::KEY_SIZE);

        std::memcpy(new_cell + LeafNode::CELL_SIZE, old_leaf.cell(cell_num),
                    (LeafNode::MAX_CELLS - cell_num) * LeafNode::CELL_SIZE);
    }

    std::uint32_t new_page_num = database.allocate_page();
    LeafNode new_leaf(pager.get_page(new_page_num));
    LeafNode old_leaf(pager.get_page(page_num));

    new_leaf.initialize();
    new_leaf.set_parent(old_leaf.get_parent());
    new_leaf.set_next_leaf(old_leaf.get_next_leaf());
    old_leaf.set_next_leaf(new_page_num);

    std::memcpy(old_leaf.cell(0), cells.data(), LeafNode::LEFT_SPLIT_COUNT * LeafNode::CELL_SIZE);
    old_leaf.set_num_cells(LeafNode::LEFT_SPLIT_COUNT);
    std::memcpy(new_leaf.cell(0), cells.data() + LeafNode::LEFT_SPLIT_COUNT * LeafNode::CELL_SIZE,
                LeafNode::RIGHT_SPLIT_COUNT * LeafNode::CELL_SIZE);
    new_leaf.set_num_cells(LeafNode::RIGHT_SPLIT_COUNT);

    if (old_leaf.is_root()) {
        create_new_root(new_page_num);
        return;
    }

    std::uint32_t parent_page_num = old_leaf.get_parent();
    update_key_in_parent(page_num);
    internal_insert(parent_page_num, new_page_num);
}

void Table::internal_insert(std::uint32_t parent_page_num, std::uint32_t child_page_num) {
    InternalNode parent(pager.get_page(parent_page_num));
    std::uint32_t num_keys = parent.get_num_keys();

    if (num_keys >= InternalNode::MAX_KEYS) {
        internal_split_and_insert(parent_page_num, child_page_num);
        return;
    }

    std::uint32_t child_max_key = get_max_key(child_page_num);
    std::uint32_t right_child_page_num = parent.get_right_child();

    std::uint32_t right_child_max_key = get_max_key(right_child_page_num);
    // Search before num_keys grows, the new last cell isn't filled in yet
    std::uint32_t index = parent.find_child(child_max_key);

    // num_keys grows first, so set_child(num_keys) below writes a cell and
    // not the right child
    parent.set_num_keys(num_keys + 1);

    if (child_max_key > right_child_max_key) {
        // New child becomes the right child, old right child gets a cell
        parent.set_child(num_keys, right_child_page_num);
        parent.set_key(num_keys, right_child_max_key);
        parent.set_right_child(child_page_num);
    } else {
        // Shift the cells after the new position one place right
        for (std::uint32_t i = num_keys; i > index; i--) {
            parent.set_child(i, parent.get_child(i - 1));
            parent.set_key(i, parent.get_key(i - 1));
        }
        parent.set_child(index, child_page_num);
        parent.set_key(index, child_max_key);
    }

    Node(pager.get_page(child_page_num)).set_parent(parent_page_num);
}

void Table::internal_split_and_insert(std::uint32_t page_num, std::uint32_t child_page_num) {
    // Collect all children with their max keys, in order, including the new one
    std::vector<std::uint32_t> children;
    std::vector<std::uint32_t> keys;
    {
        InternalNode old_node(pager.get_page(page_num));
        std::uint32_t num_keys = old_node.get_num_keys();
        for (std::uint32_t i = 0; i < num_keys; i++) {
            children.push_back(old_node.get_child(i));
            keys.push_back(old_node.get_key(i));
        }
        children.push_back(old_node.get_right_child());
    }
    keys.push_back(get_max_key(children.back()));

    std::uint32_t child_max_key = get_max_key(child_page_num);
    std::size_t index = 0;
    while (index < keys.size() && keys[index] < child_max_key) {
        index++;
    }
    children.insert(children.begin() + index, child_page_num);
    keys.insert(keys.begin() + index, child_max_key);

    std::size_t left_count = children.size() / 2;
    std::size_t right_count = children.size() - left_count;

    std::uint32_t new_page_num = database.allocate_page();
    InternalNode new_node(pager.get_page(new_page_num));
    InternalNode old_node(pager.get_page(page_num));

    // Rewrite the old node with the left half, keeping its root flag and parent
    bool was_root = old_node.is_root();
    std::uint32_t parent_page_num = old_node.get_parent();
    old_node.initialize();
    old_node.set_root(was_root);
    old_node.set_parent(parent_page_num);
    old_node.set_num_keys(static_cast<std::uint32_t>(left_count - 1));
    for (std::size_t i = 0; i < left_count - 1; i++) {
        old_node.set_child(static_cast<std::uint32_t>(i), children[i]);
        old_node.set_key(static_cast<std::uint32_t>(i), keys[i]);
    }
    old_node.set_right_child(children[left_count - 1]);

    new_node.initialize();
    new_node.set_parent(parent_page_num);
    new_node.set_num_keys(static_cast<std::uint32_t>(right_count - 1));
    for (std::size_t i = 0; i < right_count - 1; i++) {
        new_node.set_child(static_cast<std::uint32_t>(i), children[left_count + i]);
        new_node.set_key(static_cast<std::uint32_t>(i), keys[left_count + i]);
    }
    new_node.set_right_child(children.back());

    for (std::size_t i = 0; i < children.size(); i++) {
        std::uint32_t parent = i < left_count ? page_num : new_page_num;
        Node(pager.get_page(children[i])).set_parent(parent);
    }

    if (was_root) {
        create_new_root(new_page_num);
        return;
    }

    update_key_in_parent(page_num);
    internal_insert(parent_page_num, new_page_num);
}

void Table::create_new_root(std::uint32_t right_child_page_num) {
    // Copy the current root to a new page, it becomes the left child
    std::uint32_t left_child_page_num = database.allocate_page();
    char* left_data = pager.get_page(left_child_page_num);
    std::memcpy(left_data, pager.get_page(root_page_num), Pager::PAGE_SIZE);

    Node left_child(left_data);
    left_child.set_root(false);
    left_child.set_parent(root_page_num);

    // Children of the moved node now have a new parent
    if (left_child.get_type() == NodeType::Internal) {
        InternalNode left_internal(left_data);
        for (std::uint32_t i = 0; i <= left_internal.get_num_keys(); i++) {
            Node(pager.get_page(left_internal.get_child(i))).set_parent(left_child_page_num);
        }
    }

    Node(pager.get_page(right_child_page_num)).set_parent(root_page_num);

    std::uint32_t left_max_key = get_max_key(left_child_page_num);
    InternalNode root(pager.get_page(root_page_num));
    root.initialize();
    root.set_root(true);
    root.set_num_keys(1);
    root.set_child(0, left_child_page_num);
    root.set_key(0, left_max_key);
    root.set_right_child(right_child_page_num);
}

void Table::update_key_in_parent(std::uint32_t page_num) {
    std::uint32_t parent_page_num = Node(pager.get_page(page_num)).get_parent();
    std::uint32_t new_max_key = get_max_key(page_num);

    InternalNode parent(pager.get_page(parent_page_num));
    std::uint32_t index = parent.index_of_child(page_num);
    // The right child has no key in its parent
    if (index < parent.get_num_keys()) {
        parent.set_key(index, new_max_key);
    }
}

void Table::leaf_delete(std::uint32_t page_num, std::uint32_t cell_num) {
    LeafNode leaf(pager.get_page(page_num));
    bool removed_max = cell_num + 1 == leaf.get_num_cells();
    leaf.remove_cell(cell_num);

    // The root leaf can have any number of cells, even 0
    if (leaf.is_root()) {
        return;
    }

    if (leaf.get_num_cells() < LeafNode::MIN_CELLS) {
        rebalance_leaf(page_num);
    } else if (removed_max) {
        refresh_max_key(page_num);
    }
}

void Table::rebalance_leaf(std::uint32_t page_num) {
    LeafNode leaf(pager.get_page(page_num));
    std::uint32_t parent_page_num = leaf.get_parent();
    InternalNode parent(pager.get_page(parent_page_num));
    std::uint32_t index = parent.index_of_child(page_num);

    // Siblings with the same parent; a non-root node always has at least one
    bool has_left = index > 0;
    bool has_right = index < parent.get_num_keys();

    if (has_left) {
        std::uint32_t left_page_num = parent.get_child(index - 1);
        LeafNode left(pager.get_page(left_page_num));

        if (left.get_num_cells() > LeafNode::MIN_CELLS) {
            // Case 1: borrow the left sibling's last cell, it becomes our first
            std::uint32_t num_cells = leaf.get_num_cells();
            std::memmove(leaf.cell(1), leaf.cell(0), num_cells * LeafNode::CELL_SIZE);
            std::memcpy(leaf.cell(0), left.cell(left.get_num_cells() - 1), LeafNode::CELL_SIZE);
            leaf.set_num_cells(num_cells + 1);
            left.set_num_cells(left.get_num_cells() - 1);

            refresh_max_key(left_page_num);
            refresh_max_key(page_num);
            return;
        }
    }

    if (has_right) {
        std::uint32_t right_page_num = parent.get_child(index + 1);
        LeafNode right(pager.get_page(right_page_num));

        if (right.get_num_cells() > LeafNode::MIN_CELLS) {
            // Case 2: borrow the right sibling's first cell, it becomes our last
            std::uint32_t num_cells = leaf.get_num_cells();
            std::memcpy(leaf.cell(num_cells), right.cell(0), LeafNode::CELL_SIZE);
            leaf.set_num_cells(num_cells + 1);
            right.remove_cell(0);

            refresh_max_key(page_num);
            return;
        }
    }

    // Case 3: neither sibling can spare a cell, so merge with one of them.
    // The right node of the pair is emptied into the left one and freed.
    std::uint32_t left_page_num = has_left ? parent.get_child(index - 1) : page_num;
    std::uint32_t right_page_num = has_left ? page_num : parent.get_child(index + 1);
    std::uint32_t right_index = has_left ? index : index + 1;

    LeafNode left(pager.get_page(left_page_num));
    LeafNode right(pager.get_page(right_page_num));
    std::uint32_t left_cells = left.get_num_cells();
    std::uint32_t right_cells = right.get_num_cells();

    std::memcpy(left.cell(left_cells), right.cell(0), right_cells * LeafNode::CELL_SIZE);
    left.set_num_cells(left_cells + right_cells);
    left.set_next_leaf(right.get_next_leaf());

    parent.remove_child(right_index);
    database.free_page(right_page_num);

    refresh_max_key(left_page_num);
    after_child_removed(parent_page_num);
}

void Table::rebalance_internal(std::uint32_t page_num) {
    InternalNode node(pager.get_page(page_num));
    std::uint32_t parent_page_num = node.get_parent();
    InternalNode parent(pager.get_page(parent_page_num));
    std::uint32_t index = parent.index_of_child(page_num);

    bool has_left = index > 0;
    bool has_right = index < parent.get_num_keys();

    if (has_left) {
        std::uint32_t left_page_num = parent.get_child(index - 1);
        InternalNode left(pager.get_page(left_page_num));

        if (left.get_num_children() > InternalNode::MIN_CHILDREN) {
            // Case 1: the left sibling's right child becomes our first child
            std::uint32_t moved = left.get_right_child();
            std::uint32_t moved_max_key = get_max_key(moved);
            left.remove_child(left.get_num_keys());

            std::uint32_t num_keys = node.get_num_keys();
            node.set_num_keys(num_keys + 1);
            for (std::uint32_t i = num_keys; i > 0; i--) {
                node.set_child(i, node.get_child(i - 1));
                node.set_key(i, node.get_key(i - 1));
            }
            node.set_child(0, moved);
            node.set_key(0, moved_max_key);
            Node(pager.get_page(moved)).set_parent(page_num);

            refresh_max_key(left_page_num);
            return;
        }
    }

    if (has_right) {
        std::uint32_t right_page_num = parent.get_child(index + 1);
        InternalNode right(pager.get_page(right_page_num));

        if (right.get_num_children() > InternalNode::MIN_CHILDREN) {
            // Case 2: the right sibling's first child becomes our right child,
            // our old right child moves into a cell
            std::uint32_t moved = right.get_child(0);
            right.remove_child(0);

            std::uint32_t old_right = node.get_right_child();
            std::uint32_t old_right_max_key = get_max_key(old_right);
            std::uint32_t num_keys = node.get_num_keys();
            node.set_num_keys(num_keys + 1);
            node.set_child(num_keys, old_right);
            node.set_key(num_keys, old_right_max_key);
            node.set_right_child(moved);
            Node(pager.get_page(moved)).set_parent(page_num);

            refresh_max_key(page_num);
            return;
        }
    }

    // Case 3: merge. The right node of the pair moves all its children into
    // the left one: the left node's right child gets a cell, then the right
    // node's cells follow, and its right child becomes the left's right child.
    std::uint32_t left_page_num = has_left ? parent.get_child(index - 1) : page_num;
    std::uint32_t right_page_num = has_left ? page_num : parent.get_child(index + 1);
    std::uint32_t right_index = has_left ? index : index + 1;

    InternalNode left(pager.get_page(left_page_num));
    InternalNode right(pager.get_page(right_page_num));
    std::uint32_t left_keys = left.get_num_keys();
    std::uint32_t right_keys = right.get_num_keys();

    std::uint32_t old_left_right = left.get_right_child();
    std::uint32_t old_left_right_max_key = get_max_key(old_left_right);
    left.set_num_keys(left_keys + 1 + right_keys);
    left.set_child(left_keys, old_left_right);
    left.set_key(left_keys, old_left_right_max_key);
    for (std::uint32_t i = 0; i < right_keys; i++) {
        left.set_child(left_keys + 1 + i, right.get_child(i));
        left.set_key(left_keys + 1 + i, right.get_key(i));
    }
    left.set_right_child(right.get_right_child());

    for (std::uint32_t i = 0; i <= right_keys; i++) {
        Node(pager.get_page(right.get_child(i))).set_parent(left_page_num);
    }

    parent.remove_child(right_index);
    database.free_page(right_page_num);

    refresh_max_key(left_page_num);
    after_child_removed(parent_page_num);
}

void Table::after_child_removed(std::uint32_t parent_page_num) {
    InternalNode parent(pager.get_page(parent_page_num));

    if (parent.is_root()) {
        if (parent.get_num_keys() == 0) {
            collapse_root();
        }
        return;
    }

    if (parent.get_num_children() < InternalNode::MIN_CHILDREN) {
        rebalance_internal(parent_page_num);
    }
}

void Table::collapse_root() {
    char* root_data = pager.get_page(root_page_num);
    std::uint32_t child_page_num = InternalNode(root_data).get_right_child();

    // Copy the only child into the root page, the tree is now one level shorter
    std::memcpy(root_data, pager.get_page(child_page_num), Pager::PAGE_SIZE);
    Node root(root_data);
    root.set_root(true);
    root.set_parent(0);

    if (root.get_type() == NodeType::Internal) {
        InternalNode root_internal(root_data);
        for (std::uint32_t i = 0; i <= root_internal.get_num_keys(); i++) {
            Node(pager.get_page(root_internal.get_child(i))).set_parent(root_page_num);
        }
    }

    database.free_page(child_page_num);
}

void Table::refresh_max_key(std::uint32_t page_num) {
    while (true) {
        Node node(pager.get_page(page_num));
        if (node.is_root()) {
            return;
        }

        std::uint32_t parent_page_num = node.get_parent();
        InternalNode parent(pager.get_page(parent_page_num));
        std::uint32_t index = parent.index_of_child(page_num);
        if (index < parent.get_num_keys()) {
            parent.set_key(index, get_max_key(page_num));
            return;
        }

        page_num = parent_page_num;
    }
}

std::uint32_t Table::get_max_key(std::uint32_t page_num) {
    while (true) {
        Node node(pager.get_page(page_num));
        if (node.get_type() == NodeType::Leaf) {
            LeafNode leaf(node.get_data());
            std::uint32_t num_cells = leaf.get_num_cells();
            return num_cells == 0 ? 0 : leaf.get_key(num_cells - 1);
        }
        page_num = InternalNode(node.get_data()).get_right_child();
    }
}

bool Table::is_empty() {
    Node root(pager.get_page(root_page_num));
    return root.get_type() == NodeType::Leaf && LeafNode(root.get_data()).get_num_cells() == 0;
}

std::uint32_t Table::get_max_key() {
    return get_max_key(root_page_num);
}

void Table::clear() {
    free_children(root_page_num);
    initialize_root(pager, root_page_num);
}

void Table::drop() {
    free_children(root_page_num);
    database.free_page(root_page_num);
}

void Table::free_children(std::uint32_t page_num) {
    Node node(pager.get_page(page_num));
    if (node.get_type() != NodeType::Internal) {
        return;
    }

    InternalNode internal(node.get_data());
    for (std::uint32_t i = 0; i <= internal.get_num_keys(); i++) {
        std::uint32_t child = internal.get_child(i);
        free_children(child);
        database.free_page(child);
    }
}
