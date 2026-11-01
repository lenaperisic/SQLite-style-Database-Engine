#include "Cursor.h"
#include "Node.h"
#include "Table.h"

Cursor::Cursor(Table& table, std::uint32_t page_num, std::uint32_t cell_num)
    : table(table), page_num(page_num), cell_num(cell_num) {
    LeafNode leaf(table.pager.get_page(page_num));
    end_of_table = cell_num >= leaf.get_num_cells() && leaf.get_next_leaf() == 0;
}

Cursor Cursor::table_start(Table& table) {
    return table_find(table, 0);
}

Cursor Cursor::table_find(Table& table, std::uint32_t key) {
    std::uint32_t page_num = table.root_page_num;

    while (true) {
        Node node(table.pager.get_page(page_num));
        if (node.get_type() == NodeType::Leaf) {
            LeafNode leaf(node.get_data());
            return Cursor(table, page_num, leaf.find_cell(key));
        }

        InternalNode internal(node.get_data());
        page_num = internal.get_child(internal.find_child(key));
    }
}

char* Cursor::value() {
    return LeafNode(table.pager.get_page(page_num)).value(cell_num);
}

std::uint32_t Cursor::get_key() {
    return LeafNode(table.pager.get_page(page_num)).get_key(cell_num);
}

bool Cursor::is_at(std::uint32_t key) {
    LeafNode leaf(table.pager.get_page(page_num));
    return cell_num < leaf.get_num_cells() && leaf.get_key(cell_num) == key;
}

void Cursor::advance() {
    LeafNode leaf(table.pager.get_page(page_num));
    cell_num++;

    if (cell_num >= leaf.get_num_cells()) {
        std::uint32_t next_leaf = leaf.get_next_leaf();
        if (next_leaf == 0) {
            end_of_table = true;
        } else {
            page_num = next_leaf;
            cell_num = 0;
        }
    }
}

bool Cursor::is_end() const {
    return end_of_table;
}

std::uint32_t Cursor::get_page_num() const {
    return page_num;
}

std::uint32_t Cursor::get_cell_num() const {
    return cell_num;
}
