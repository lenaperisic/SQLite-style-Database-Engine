#include "Value.h"
#include "ExceptionHandler.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

Value Value::integer(std::int64_t value) {
    return Value(Data(value));
}

Value Value::real(double value) {
    return Value(Data(value));
}

Value Value::text(std::string value) {
    return Value(Data(std::move(value)));
}

Value Value::blob(std::vector<std::uint8_t> value) {
    return Value(Data(std::move(value)));
}

DataType Value::get_type() const {
    // The variant's alternatives are listed in the same order as DataType
    return static_cast<DataType>(data.index());
}

std::int64_t Value::as_integer() const {
    return std::get<std::int64_t>(data);
}

double Value::as_real() const {
    return std::get<double>(data);
}

const std::string& Value::as_text() const {
    return std::get<std::string>(data);
}

const std::vector<std::uint8_t>& Value::as_blob() const {
    return std::get<std::vector<std::uint8_t>>(data);
}

std::string Value::to_string() const {
    switch (get_type()) {
        case DataType::Null:
            return "NULL";
        case DataType::Integer:
            return std::to_string(as_integer());
        case DataType::Real: {
            std::ostringstream out;
            out << std::setprecision(15) << as_real();
            std::string result = out.str();
            // Show 2.0 instead of 2, so it doesn't look like an integer
            if (result.find_first_of(".eni") == std::string::npos) {
                result += ".0";
            }
            return result;
        }
        case DataType::Text:
            return as_text();
        case DataType::Blob: {
            std::ostringstream out;
            out << "x'" << std::hex << std::uppercase << std::setfill('0');
            for (std::uint8_t byte : as_blob()) {
                out << std::setw(2) << static_cast<int>(byte);
            }
            out << "'";
            return out.str();
        }
    }
    return "";
}

std::optional<Value> Value::parse_number(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t digits_start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    bool all_digits = digits_start < text.size();
    bool has_digit = false;
    for (std::size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        has_digit = has_digit || digit;
        if (i >= digits_start && !digit) {
            all_digits = false;
        }
        // Only characters that can appear in a number; this also keeps
        // "inf" and "nan", which stod would accept, as text
        if (!digit && c != '.' && c != 'e' && c != 'E' && c != '-' && c != '+') {
            return std::nullopt;
        }
    }
    if (!has_digit) {
        return std::nullopt;
    }

    if (all_digits) {
        try {
            return integer(std::stoll(text));
        }
        catch (const std::out_of_range&) {
            return real(std::stod(text));
        }
    }

    try {
        std::size_t used = 0;
        double value = std::stod(text, &used);
        if (used == text.size()) {
            return real(value);
        }
    }
    catch (const std::exception&) {
        // e.g. "1-2" or "e5", not a number
    }
    return std::nullopt;
}

Value Value::with_affinity(DataType affinity) const {
    DataType type = get_type();
    if (type == DataType::Null || type == DataType::Blob) {
        return *this;
    }

    switch (affinity) {
        case DataType::Integer: {
            Value number = *this;
            if (type == DataType::Text) {
                std::optional<Value> parsed = parse_number(as_text());
                if (!parsed) {
                    return *this;   // stays text, SQLite doesn't refuse it
                }
                number = *parsed;
            }
            // A real with nothing after the point is stored as an integer
            if (number.get_type() == DataType::Real) {
                double value = number.as_real();
                if (std::floor(value) == value && value >= -9223372036854775808.0
                    && value < 9223372036854775808.0) {
                    return integer(static_cast<std::int64_t>(value));
                }
            }
            return number;
        }
        case DataType::Real:
            if (type == DataType::Integer) {
                return real(static_cast<double>(as_integer()));
            }
            // SQLite stores a whole REAL as an integer, so -0.0 comes back as 0.0
            if (type == DataType::Real && as_real() == 0.0) {
                return real(0.0);
            }
            if (type == DataType::Text) {
                std::optional<Value> parsed = parse_number(as_text());
                if (parsed) {
                    return parsed->with_affinity(DataType::Real);
                }
            }
            return *this;
        case DataType::Text:
            return text(to_string());
        default:
            return *this;
    }
}

// Byte counts of serial types 1..6
static const std::size_t INTEGER_SIZES[] = {0, 1, 2, 3, 4, 6, 8};

std::uint64_t Value::serial_type() const {
    switch (get_type()) {
        case DataType::Null:
            return 0;
        case DataType::Integer: {
            std::int64_t value = as_integer();
            if (value == 0) return 8;
            if (value == 1) return 9;
            // Smallest size whose signed range holds the value
            for (std::uint64_t type = 1; type <= 5; type++) {
                std::size_t bits = INTEGER_SIZES[type] * 8;
                std::int64_t limit = std::int64_t(1) << (bits - 1);
                if (value >= -limit && value < limit) {
                    return type;
                }
            }
            return 6;
        }
        case DataType::Real:
            return 7;
        case DataType::Text:
            return 13 + 2 * static_cast<std::uint64_t>(as_text().size());
        case DataType::Blob:
            return 12 + 2 * static_cast<std::uint64_t>(as_blob().size());
    }
    return 0;
}

std::size_t Value::body_size(std::uint64_t serial_type) {
    if (serial_type >= 1 && serial_type <= 6) {
        return INTEGER_SIZES[serial_type];
    }
    if (serial_type == 7) {
        return 8;
    }
    if (serial_type >= 12) {
        return static_cast<std::size_t>((serial_type - 12) / 2);
    }
    // 0 (NULL), 8 and 9 (the integers 0 and 1) have no body
    return 0;
}

// Appends the lowest `size` bytes of value, most significant first
static void write_big_endian(std::vector<std::uint8_t>& out, std::uint64_t value, std::size_t size) {
    for (std::size_t i = size; i > 0; i--) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
    }
}

static std::uint64_t read_big_endian(const std::uint8_t* data, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

void Value::write_body(std::vector<std::uint8_t>& out) const {
    std::uint64_t type = serial_type();
    switch (get_type()) {
        case DataType::Null:
            break;
        case DataType::Integer:
            // Two's complement, cut down to body_size bytes
            write_big_endian(out, static_cast<std::uint64_t>(as_integer()), body_size(type));
            break;
        case DataType::Real: {
            std::uint64_t bits;
            double value = as_real();
            std::memcpy(&bits, &value, sizeof(bits));
            write_big_endian(out, bits, 8);
            break;
        }
        case DataType::Text:
            out.insert(out.end(), as_text().begin(), as_text().end());
            break;
        case DataType::Blob:
            out.insert(out.end(), as_blob().begin(), as_blob().end());
            break;
    }
}

Value Value::read_body(std::uint64_t serial_type, const std::uint8_t* data) {
    std::size_t size = body_size(serial_type);

    if (serial_type == 0) {
        return Value();
    }
    if (serial_type == 8 || serial_type == 9) {
        return integer(serial_type == 9 ? 1 : 0);
    }
    if (serial_type <= 6) {
        std::uint64_t raw = read_big_endian(data, size);
        // Sign-extend: if the top bit of the stored bytes is set, fill the rest with 1s
        if (size < 8 && (raw >> (size * 8 - 1)) & 1) {
            raw |= ~std::uint64_t(0) << (size * 8);
        }
        return integer(static_cast<std::int64_t>(raw));
    }
    if (serial_type == 7) {
        std::uint64_t bits = read_big_endian(data, 8);
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        return real(value);
    }
    if (serial_type >= 12 && serial_type % 2 == 0) {
        return blob(std::vector<std::uint8_t>(data, data + size));
    }
    if (serial_type >= 13) {
        return text(std::string(reinterpret_cast<const char*>(data), size));
    }

    // 10 and 11 are reserved
    throw DatabaseFileException("Unknown serial type " + std::to_string(serial_type) + ". Corrupt file.");
}
