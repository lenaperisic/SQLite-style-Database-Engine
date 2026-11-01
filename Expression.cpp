#include "Expression.h"
#include "ExceptionHandler.h"

#include <cctype>
#include <cmath>
#include <limits>

static const std::int64_t INT_MIN64 = std::numeric_limits<std::int64_t>::min();
static const std::int64_t INT_MAX64 = std::numeric_limits<std::int64_t>::max();

static std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

void bind(const Expr& expr, const TableSchema& schema, bool allow_aggregates) {
    if (expr.kind == ExprKind::Column) {
        std::string name = to_lower(expr.name);
        for (std::size_t i = 0; i < schema.columns.size(); i++) {
            if (to_lower(schema.columns[i].name) == name) {
                expr.column_index = static_cast<int>(i);
                return;
            }
        }
        if (name == "rowid") {
            expr.column_index = -1;
            return;
        }
        throw NoSuchColumnException(expr.name);
    }

    if (expr.kind == ExprKind::Aggregate) {
        if (!allow_aggregates) {
            throw SyntaxErrorException("misuse of aggregate " + expr.name + "()");
        }
        // SUM(COUNT(*)) makes no sense
        allow_aggregates = false;
    }

    for (const auto& arg : expr.args) {
        bind(*arg, schema, allow_aggregates);
    }
}

bool contains_aggregate(const Expr& expr) {
    if (expr.kind == ExprKind::Aggregate) {
        return true;
    }
    for (const auto& arg : expr.args) {
        if (contains_aggregate(*arg)) {
            return true;
        }
    }
    return false;
}

bool uses_column_outside_aggregate(const Expr& expr) {
    if (expr.kind == ExprKind::Column) {
        return true;
    }
    if (expr.kind == ExprKind::Aggregate) {
        return false;
    }
    for (const auto& arg : expr.args) {
        if (uses_column_outside_aggregate(*arg)) {
            return true;
        }
    }
    return false;
}

void collect_aggregates(const Expr& expr, std::vector<const Expr*>& out) {
    if (expr.kind == ExprKind::Aggregate) {
        out.push_back(&expr);
        return;
    }
    for (const auto& arg : expr.args) {
        collect_aggregates(*arg, out);
    }
}

// Numbers stay numbers, text becomes the number it holds (or 0), blobs 0
static Value to_numeric(const Value& value) {
    switch (value.get_type()) {
        case DataType::Integer:
        case DataType::Real:
        case DataType::Null:
            return value;
        case DataType::Text: {
            std::optional<Value> number = Value::parse_number(value.as_text());
            return number ? *number : Value::integer(0);
        }
        default:
            return Value::integer(0);
    }
}

static double as_double(const Value& number) {
    return number.get_type() == DataType::Integer ? static_cast<double>(number.as_integer()) : number.as_real();
}

bool is_true(const Value& value) {
    Value number = to_numeric(value);
    switch (number.get_type()) {
        case DataType::Integer: return number.as_integer() != 0;
        case DataType::Real: return number.as_real() != 0.0;
        default: return false;
    }
}


static int type_rank(const Value& value) {
    switch (value.get_type()) {
        case DataType::Null: return 0;
        case DataType::Integer:
        case DataType::Real: return 1;
        case DataType::Text: return 2;
        default: return 3;
    }
}

int compare_values(const Value& a, const Value& b) {
    int rank_a = type_rank(a);
    int rank_b = type_rank(b);
    if (rank_a != rank_b) {
        return rank_a < rank_b ? -1 : 1;
    }

    switch (rank_a) {
        case 0:
            return 0;
        case 1: {
            if (a.get_type() == DataType::Integer && b.get_type() == DataType::Integer) {
                std::int64_t x = a.as_integer();
                std::int64_t y = b.as_integer();
                return x < y ? -1 : (x > y ? 1 : 0);
            }
            long double x = a.get_type() == DataType::Integer ? a.as_integer() : a.as_real();
            long double y = b.get_type() == DataType::Integer ? b.as_integer() : b.as_real();
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        case 2: {

            int c = a.as_text().compare(b.as_text());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        default: {
            const auto& x = a.as_blob();
            const auto& y = b.as_blob();
            return x < y ? -1 : (x == y ? 0 : 1);
        }
    }
}

// ---- arithmetic: integers while they fit, otherwise reals (like SQLite) ----

static bool add_overflows(std::int64_t a, std::int64_t b) {
    return (b > 0 && a > INT_MAX64 - b) || (b < 0 && a < INT_MIN64 - b);
}

static bool sub_overflows(std::int64_t a, std::int64_t b) {
    return (b < 0 && a > INT_MAX64 + b) || (b > 0 && a < INT_MIN64 + b);
}

static bool mul_overflows(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) {
        return false;
    }
    if ((a == -1 && b == INT_MIN64) || (b == -1 && a == INT_MIN64)) {
        return true;
    }
    std::int64_t product = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    return product / b != a;
}

static Value arithmetic(const std::string& op, const Value& left, const Value& right) {
    if (left.get_type() == DataType::Null || right.get_type() == DataType::Null) {
        return Value();
    }
    Value a = to_numeric(left);
    Value b = to_numeric(right);

    if (a.get_type() == DataType::Integer && b.get_type() == DataType::Integer) {
        std::int64_t x = a.as_integer();
        std::int64_t y = b.as_integer();
        if (op == "+" && !add_overflows(x, y)) return Value::integer(x + y);
        if (op == "-" && !sub_overflows(x, y)) return Value::integer(x - y);
        if (op == "*" && !mul_overflows(x, y)) return Value::integer(x * y);
        if (op == "/") {
            if (y == 0) return Value();
            if (!(x == INT_MIN64 && y == -1)) return Value::integer(x / y);
        }
        if (op == "%") {
            if (y == 0) return Value();
            return Value::integer(y == -1 ? 0 : x % y);
        }
        // Overflow, fall through to real arithmetic
    }

    double x = as_double(a);
    double y = as_double(b);
    if (op == "+") return Value::real(x + y);
    if (op == "-") return Value::real(x - y);
    if (op == "*") return Value::real(x * y);
    if (op == "/") return y == 0.0 ? Value() : Value::real(x / y);

    // % works on whole numbers, the result is real if an operand was real
    std::int64_t ix = static_cast<std::int64_t>(x);
    std::int64_t iy = static_cast<std::int64_t>(y);
    if (iy == 0) return Value();
    return Value::real(static_cast<double>(iy == -1 ? 0 : ix % iy));
}

// Three-valued logic: true, false, or unknown (NULL)
enum class Truth { False, True, Unknown };

static Truth truth(const Value& value) {
    if (value.get_type() == DataType::Null) {
        return Truth::Unknown;
    }
    return is_true(value) ? Truth::True : Truth::False;
}

static Value from_truth(Truth t) {
    if (t == Truth::Unknown) {
        return Value();
    }
    return Value::integer(t == Truth::True ? 1 : 0);
}

Value evaluate(const Expr& expr, std::uint32_t rowid, const std::vector<Value>& row,
               const std::map<const Expr*, Value>* aggregates) {
    switch (expr.kind) {
        case ExprKind::Literal:
            return expr.value;

        case ExprKind::Column:
            if (expr.column_index < 0) {
                return Value::integer(rowid);
            }
            return static_cast<std::size_t>(expr.column_index) < row.size() ? row[expr.column_index] : Value();

        case ExprKind::Aggregate:
            if (aggregates) {
                auto it = aggregates->find(&expr);
                if (it != aggregates->end()) {
                    return it->second;
                }
            }
            throw SyntaxErrorException("misuse of aggregate " + expr.name + "()");

        case ExprKind::IsNull: {
            bool is_null = evaluate(*expr.args[0], rowid, row, aggregates).get_type() == DataType::Null;
            return Value::integer((expr.op == "IS NULL") == is_null ? 1 : 0);
        }

        case ExprKind::Unary: {
            Value operand = evaluate(*expr.args[0], rowid, row, aggregates);
            if (expr.op == "NOT") {
                Truth t = truth(operand);
                return from_truth(t == Truth::Unknown ? t : (t == Truth::True ? Truth::False : Truth::True));
            }
            if (expr.op == "+") {
                return operand;
            }
            return arithmetic("-", Value::integer(0), operand);
        }

        case ExprKind::Binary: {
            Value left = evaluate(*expr.args[0], rowid, row, aggregates);

            if (expr.op == "AND" || expr.op == "OR") {
                Truth a = truth(left);
                // Short circuit: false AND x = false, true OR x = true
                if (expr.op == "AND" && a == Truth::False) return Value::integer(0);
                if (expr.op == "OR" && a == Truth::True) return Value::integer(1);
                Truth b = truth(evaluate(*expr.args[1], rowid, row, aggregates));
                if (expr.op == "AND") {
                    if (b == Truth::False) return Value::integer(0);
                    return from_truth(a == Truth::Unknown || b == Truth::Unknown ? Truth::Unknown : Truth::True);
                }
                if (b == Truth::True) return Value::integer(1);
                return from_truth(a == Truth::Unknown || b == Truth::Unknown ? Truth::Unknown : Truth::False);
            }

            Value right = evaluate(*expr.args[1], rowid, row, aggregates);

            if (expr.op == "+" || expr.op == "-" || expr.op == "*" || expr.op == "/" || expr.op == "%") {
                return arithmetic(expr.op, left, right);
            }

            // Comparison: anything compared with NULL is unknown
            if (left.get_type() == DataType::Null || right.get_type() == DataType::Null) {
                return Value();
            }
            int c = compare_values(left, right);
            bool result;
            if (expr.op == "=") result = c == 0;
            else if (expr.op == "!=") result = c != 0;
            else if (expr.op == "<") result = c < 0;
            else if (expr.op == "<=") result = c <= 0;
            else if (expr.op == ">") result = c > 0;
            else result = c >= 0;   // ">="
            return Value::integer(result ? 1 : 0);
        }
    }
    return Value();
}

void Aggregator::add(std::uint32_t rowid, const std::vector<Value>& row) {
    // COUNT(*) counts rows, the others look at the argument and skip NULLs
    if (expr.args.empty()) {
        count++;
        return;
    }

    Value value = evaluate(*expr.args[0], rowid, row);
    if (value.get_type() == DataType::Null) {
        return;
    }
    count++;

    if (expr.name == "SUM" || expr.name == "AVG") {
        Value number = to_numeric(value);
        if (number.get_type() == DataType::Integer) {
            std::int64_t n = number.as_integer();
            if (!sum_is_real && !add_overflows(int_sum, n)) {
                int_sum += n;
            } else {
                // Too big for an integer sum: carry on with reals
                start_real_sum();
                add_integer_to_real(n);
            }
        } else {
            start_real_sum();
            add_real(number.as_real());
        }
    } else if (expr.name == "MIN" || expr.name == "MAX") {
        int c = has_best ? compare_values(value, best) : 0;
        if (!has_best || (expr.name == "MIN" ? c < 0 : c > 0)) {
            best = value;
            has_best = true;
        }
    }
}

Value Aggregator::result() const {
    if (expr.name == "COUNT") {
        return Value::integer(count);
    }
    if (count == 0) {
        return Value();     // SUM / AVG / MIN / MAX of no rows is NULL
    }
    if (expr.name == "SUM") {
        return sum_is_real ? Value::real(real_total()) : Value::integer(int_sum);
    }
    if (expr.name == "AVG") {
        double sum = sum_is_real ? real_total() : static_cast<double>(int_sum);
        return Value::real(sum / static_cast<double>(count));
    }
    return best;
}

// Integers above 2^52 don't fit in a double exactly, so they are added in
// two parts: the low bits and the rest
static const std::int64_t EXACT_DOUBLE_LIMIT = 4503599627370496LL;    // 2^52

void Aggregator::start_real_sum() {
    if (sum_is_real) {
        return;
    }
    sum_is_real = true;
    if (int_sum <= -EXACT_DOUBLE_LIMIT || int_sum >= EXACT_DOUBLE_LIMIT) {
        std::int64_t low = int_sum % 16384;
        real_sum = static_cast<double>(int_sum - low);
        real_error = static_cast<double>(low);
    } else {
        real_sum = static_cast<double>(int_sum);
        real_error = 0;
    }
}

void Aggregator::add_real(double value) {
    double sum = real_sum + value;
    // Whatever rounding lost from the smaller of the two goes into the error
    if (std::fabs(real_sum) > std::fabs(value)) {
        real_error += (real_sum - sum) + value;
    } else {
        real_error += (value - sum) + real_sum;
    }
    real_sum = sum;
}

void Aggregator::add_integer_to_real(std::int64_t value) {
    if (value <= -EXACT_DOUBLE_LIMIT || value >= EXACT_DOUBLE_LIMIT) {
        std::int64_t low = value % 16384;
        add_real(static_cast<double>(low));
        add_real(static_cast<double>(value - low));
    } else {
        add_real(static_cast<double>(value));
    }
}
