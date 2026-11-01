#include "Row.h"
#include "ExceptionHandler.h"
#include "Varint.h"

#include <cstring>
#include <iostream>

Row::Row(std::uint32_t id, std::vector<Value> values)
    : id(id), values(std::move(values)) {
    std::size_t size = to_record().size();
    if (size > MAX_RECORD_SIZE) {
        throw RowTooLargeException(size, MAX_RECORD_SIZE);
    }
}

std::vector<std::uint8_t> Row::to_record() const {
    std::vector<std::uint8_t> types;
    for (const Value& value : values) {
        write_varint(types, value.serial_type());
    }

    // The header size counts itself too, and its own varint can get longer
    // as the number grows, so grow it until it fits
    std::size_t header_size = types.size() + 1;
    while (types.size() + varint_size(header_size) != header_size) {
        header_size = types.size() + varint_size(header_size);
    }

    std::vector<std::uint8_t> record;
    write_varint(record, header_size);
    record.insert(record.end(), types.begin(), types.end());
    for (const Value& value : values) {
        value.write_body(record);
    }
    return record;
}

std::vector<Value> Row::from_record(const std::uint8_t* data, std::size_t size) {
    std::size_t read;
    std::uint64_t header_size = read_varint(data, size, read);
    if (header_size > size || header_size < read) {
        throw DatabaseFileException("Bad record header size. Corrupt file.");
    }

    // Serial types are in the header, the bodies follow it in the same order
    std::vector<Value> values;
    std::size_t header_pos = read;
    std::size_t body_pos = static_cast<std::size_t>(header_size);
    while (header_pos < header_size) {
        std::uint64_t type = read_varint(data + header_pos, header_size - header_pos, read);
        header_pos += read;

        std::size_t body = Value::body_size(type);
        if (body_pos + body > size) {
            throw DatabaseFileException("Record body runs past its end. Corrupt file.");
        }
        values.push_back(Value::read_body(type, data + body_pos));
        body_pos += body;
    }

    if (body_pos != size) {
        throw DatabaseFileException("Record has extra bytes. Corrupt file.");
    }
    return values;
}

void Row::serialize(char* destination) const {
    std::vector<std::uint8_t> record = to_record();
    // Length first (big-endian, like the record), then the record
    destination[0] = static_cast<char>(record.size() >> 8);
    destination[1] = static_cast<char>(record.size() & 0xFF);
    std::memcpy(destination + LENGTH_SIZE, record.data(), record.size());
}

Row Row::deserialize(std::uint32_t id, const char* source) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(source);
    std::size_t size = (static_cast<std::size_t>(bytes[0]) << 8) | bytes[1];
    if (size > MAX_RECORD_SIZE) {
        throw DatabaseFileException("Record length " + std::to_string(size) + " too big. Corrupt file.");
    }

    Row row;
    row.id = id;
    row.values = from_record(bytes + LENGTH_SIZE, size);
    return row;
}

std::uint32_t Row::get_id() const {
    return id;
}

const std::vector<Value>& Row::get_values() const {
    return values;
}

void Row::print() const {
    std::cout << "(";
    for (std::size_t i = 0; i < values.size(); i++) {
        std::cout << (i > 0 ? ", " : "") << values[i].to_string();
    }
    std::cout << ")\n";
}
