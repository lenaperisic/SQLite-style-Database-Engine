#ifndef STATEMENT_H
#define STATEMENT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Expression.h"
#include "Schema.h"
#include "Value.h"

class Database;

enum class StatementType {
    CreateTable,    // CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)
    DropTable,      // DROP TABLE users
    Insert,         // INSERT INTO users VALUES (1, 'ana')
    Select,         // SELECT name, age + 1 FROM users WHERE age > 20 ORDER BY name LIMIT 10
    Update,         // UPDATE users SET age = age + 1 WHERE id = 1
    Delete,         // DELETE FROM users WHERE age < 18
    Begin,          // BEGIN
    Commit,         // COMMIT
    Rollback        // ROLLBACK
};

// One item after SELECT: * or an expression
struct SelectItem {
    bool star = false;
    std::unique_ptr<Expr> expr;
};

struct OrderTerm {
    std::unique_ptr<Expr> expr;
    bool descending = false;
};

// column = expression, in UPDATE ... SET
struct Assignment {
    std::string column;
    std::unique_ptr<Expr> expr;
};

struct Statement {
    StatementType type;
    std::string table_name;
    TableSchema schema;                     // CREATE TABLE
    std::vector<Value> values;              // INSERT
    std::vector<SelectItem> select_items;   // SELECT
    std::unique_ptr<Expr> where;            // SELECT / UPDATE / DELETE, empty = every row
    std::vector<OrderTerm> order_by;        // SELECT
    std::optional<std::int64_t> limit;      // SELECT
    std::int64_t offset = 0;                // SELECT
    std::vector<Assignment> assignments;    // UPDATE
};

// The "SQL compiler": turns a line of input into a Statement (see Parser).
// Throws UnrecognizedStatementException if the first word isn't a keyword,
// SyntaxErrorException / DuplicateColumnException if the rest is wrong.
Statement prepare_statement(const std::string& line);

// The "virtual machine": runs a prepared Statement on the database.
// Outside BEGIN ... COMMIT every statement that changes something is its
// own transaction: it's saved to disk when it succeeds and undone when it
// fails. Throws the exceptions in ExceptionHandler.h.
void execute_statement(const Statement& statement, Database& database);

#endif //STATEMENT_H
