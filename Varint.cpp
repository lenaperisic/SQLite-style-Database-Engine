#include "Varint.h"
#include "ExceptionHandler.h"

#include <algorithm>

std::size_t varint_size(std::uint64_t value) {
    std::size_t size = 1;
    // Each extra byte adds 7 bits, the 9th byte adds 8
    while (size < 9 && (value >> (7 * size)) != 0) {
        size++;
    }
    return size;
}

void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (varint_size(value) == 9) {
        // 8 bytes of 7 bits (high 56 bits), then a full last byte
        std::uint8_t last = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
        std::uint8_t bytes[8];
        for (int i = 7; i >= 0; i--) {
            bytes[i] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
            value >>= 7;
        }
        out.insert(out.end(), bytes, bytes + 8);
        out.push_back(last);
        return;
    }

    // Collect 7-bit groups from the lowest, then write them highest first
    std::vector<std::uint8_t> groups;
    do {
        groups.push_back(static_cast<std::uint8_t>(value & 0x7F));
        value >>= 7;
    } while (value != 0);
    std::reverse(groups.begin(), groups.end());

    for (std::size_t i = 0; i < groups.size(); i++) {
        bool more = i + 1 < groups.size();
        out.push_back(static_cast<std::uint8_t>(groups[i] | (more ? 0x80 : 0)));
    }
}

std::uint64_t read_varint(const std::uint8_t* data, std::size_t size, std::size_t& bytes_read) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 9; i++) {
        if (i >= size) {
            throw DatabaseFileException("Varint runs past the end of the record. Corrupt file.");
        }
        if (i == 8) {
            value = (value << 8) | data[i];
            bytes_read = 9;
            return value;
        }
        value = (value << 7) | (data[i] & 0x7F);
        if ((data[i] & 0x80) == 0) {
            bytes_read = i + 1;
            return value;
        }
    }
    return value;  // not reached
}
