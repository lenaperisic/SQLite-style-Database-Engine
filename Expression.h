#ifndef EXPRESSION_H
#define EXPRESSION_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Schema.h"
#include "Value.h"

enum class ExprKind {
    Literal,    // 42, 'ana', NULL
    Column,     // age, rowid
    Unary,      // -x, +x, NOT x
    Binary,     // a + b, a = b, a AND b, ...
    IsNull,     // x IS NULL, x IS NOT NULL
    Aggregate   // COUNT(*), COUNT(x), SUM(x), MIN(x), MAX(x), AVG(x)
};

/* A node of an expression tree, built by the Parser.
   "age > 20 AND name = 'ana'" becomes

              AND
            /     \
           >       =
          / \     / \
        age  20 name 'ana'
*/
struct Expr {
    ExprKind kind;
    Value value;                                // Literal
    std::string name;                           // Column name, or function name (upper case)
    std::string op;                             // Unary / Binary operator, IsNull: "IS NULL" / "IS NOT NULL"
    std::vector<std::unique_ptr<Expr>> args;    // operands; COUNT(*) has none

    // Set by bind(): which value of the row a Column means, -1 = the row id
    mutable int column_index = 0;
};

// Finds the columns an expression uses in the table.
// Throws NoSuchColumnException, or SyntaxErrorException for an aggregate
// where it isn't allowed (e.g. in WHERE).
void bind(const Expr& expr, const TableSchema& schema, bool allow_aggregates);

bool contains_aggregate(const Expr& expr);
// True if a column is used outside of any aggregate
bool uses_column_outside_aggregate(const Expr& expr);
// Every aggregate node in the expression
void collect_aggregates(const Expr& expr, std::vector<const Expr*>& out);

// Value of the expression for one row: its id and the values of its columns.
// Aggregates take their result from `aggregates`.
Value evaluate(const Expr& expr, std::uint32_t rowid, const std::vector<Value>& row,
               const std::map<const Expr*, Value>* aggregates = nullptr);

// How WHERE reads a value: NULL and 0 are false, other numbers true,
// text counts as the number it starts with ('3' true, 'abc' false)
bool is_true(const Value& value);

// Order used by ORDER BY, MIN and MAX, like SQLite:
// NULL < numbers < text < blobs; numbers by value, text and blobs by bytes.
// Returns <0, 0 or >0.
int compare_values(const Value& a, const Value& b);

// Collects the rows for one aggregate (COUNT, SUM, ...)
class Aggregator {
public:
    explicit Aggregator(const Expr& expr) : expr(expr) {}

    void add(std::uint32_t rowid, const std::vector<Value>& row);
    Value result() const;

private:
    // Real sums use Kahan-Babuska-Neumaier summation, like SQLite: besides
    // the sum it keeps the rounding error lost so far and adds it at the end
    void start_real_sum();
    void add_real(double value);
    void add_integer_to_real(std::int64_t value);
    double real_total() const { return real_sum + real_error; }

    const Expr& expr;
    std::int64_t count = 0;
    std::int64_t int_sum = 0;
    double real_sum = 0;
    double real_error = 0;
    bool sum_is_real = false;
    bool has_best = false;
    Value best;     // MIN / MAX so far
};

#endif //EXPRESSION_H
