#ifndef VALUE_H
#define VALUE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// The five storage classes of SQLite
enum class DataType {
    Null,
    Integer,    // 64-bit signed
    Real,       // 64-bit double
    Text,
    Blob        // raw bytes
};

// One value in a row. std::variant holds exactly one of the types and
// remembers which one, like a union with a built-in enum.
class Value {
public:
    // NULL
    Value() = default;
    static Value integer(std::int64_t value);
    static Value real(double value);
    static Value text(std::string value);
    static Value blob(std::vector<std::uint8_t> value);

    DataType get_type() const;

    // Each throws std::bad_variant_access if the value has another type
    std::int64_t as_integer() const;
    double as_real() const;
    const std::string& as_text() const;
    const std::vector<std::uint8_t>& as_blob() const;

    // How select prints it: NULL, 42, 3.5, text as is, x'0AFF'
    std::string to_string() const;

    // "42" -> integer, "-3.5" / "1e10" -> real, anything else -> nothing.
    // An integer too big for 64 bits becomes a real, like in SQLite.
    static std::optional<Value> parse_number(const std::string& text);

    // Converts the value toward a column's type when it's stored, like
    // SQLite's column affinity (it's a preference, not a strict check):
    //   INTEGER: numeric text -> number, whole real -> integer
    //   REAL:    integer or numeric text -> real
    //   TEXT:    integer or real -> text
    //   BLOB:    no conversion
    // NULL stays NULL in every column.
    Value with_affinity(DataType affinity) const;

    // ---- SQLite record format ----
    // Serial type: says the type and how many bytes the body takes
    //   0 NULL, 1..6 integer of 1/2/3/4/6/8 bytes, 7 real,
    //   8 the integer 0, 9 the integer 1,
    //   12 + 2n blob of n bytes, 13 + 2n text of n bytes
    std::uint64_t serial_type() const;
    static std::size_t body_size(std::uint64_t serial_type);
    // Appends the value's bytes (big-endian numbers)
    void write_body(std::vector<std::uint8_t>& out) const;
    // Reads a value of this serial type from data
    static Value read_body(std::uint64_t serial_type, const std::uint8_t* data);

private:
    using Data = std::variant<std::monostate,             // NULL
                              std::int64_t,               // INTEGER
                              double,                     // REAL
                              std::string,                // TEXT
                              std::vector<std::uint8_t>>; // BLOB

    explicit Value(Data data) : data(std::move(data)) {}

    Data data;
};

#endif //VALUE_H
