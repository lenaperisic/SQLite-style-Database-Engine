#ifndef VARINT_H
#define VARINT_H

#include <cstddef>
#include <cstdint>
#include <vector>

// SQLite's variable-length integer: 1 to 9 bytes, big-endian. Each of the
// first 8 bytes holds 7 bits of the value, and its high bit means "more bytes
// follow". If there is a 9th byte, it holds 8 bits.
// Small numbers take 1 byte (0..127), the largest 64-bit numbers take 9.

std::size_t varint_size(std::uint64_t value);
void write_varint(std::vector<std::uint8_t>& out, std::uint64_t value);
// Reads a varint from data (at most `size` bytes available).
// Sets bytes_read. Throws DatabaseFileException if it runs past the end.
std::uint64_t read_varint(const std::uint8_t* data, std::size_t size, std::size_t& bytes_read);

#endif //VARINT_H
