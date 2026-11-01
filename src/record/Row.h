#ifndef ROW_H
#define ROW_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Value.h"

// One row of the table: an id (the key in the B+ tree, like SQLite's rowid)
// and any number of values of any type.
//
// On disk the values are stored in SQLite's record format:
//   [ header size ][ serial type 1 ][ serial type 2 ]... [ body 1 ][ body 2 ]...
// (all header numbers are varints). The id isn't part of the record, it's
// the cell's key.
//
// Leaf cells still have a fixed-size slot for the row, so the record has to
// fit in it:  [ record length: 2 ][ record: up to 291 bytes ]
class Row {
public:
    static constexpr std::size_t ROW_SIZE = 293;
    static constexpr std::size_t LENGTH_SIZE = 2;
    static constexpr std::size_t MAX_RECORD_SIZE = ROW_SIZE - LENGTH_SIZE;

    Row() = default;
    // Throws RowTooLargeException if the record doesn't fit in the slot
    Row(std::uint32_t id, std::vector<Value> values);

    // Writes the record into the ROW_SIZE bytes at destination
    void serialize(char* destination) const;
    // Reads a row back; the id comes from the cell's key
    static Row deserialize(std::uint32_t id, const char* source);

    // The values in SQLite record format
    std::vector<std::uint8_t> to_record() const;
    // Throws DatabaseFileException if the bytes aren't a valid record
    static std::vector<Value> from_record(const std::uint8_t* data, std::size_t size);

    std::uint32_t get_id() const;
    const std::vector<Value>& get_values() const;
    std::vector<Value>& get_values() { return values; }

    // Prints the values: (1, alice, 25, NULL)
    void print() const;

private:
    std::uint32_t id = 0;
    std::vector<Value> values;
};

#endif //ROW_H
