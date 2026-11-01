#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>

#include "Statement.h"
#include "Tokenizer.h"

// Builds a Statement from one line of SQL. Keywords are case-insensitive,
// a ';' at the end is optional.
//
//   CREATE TABLE name ( column type [PRIMARY KEY] , ... )
//   DROP TABLE name
//   INSERT INTO name VALUES ( value , ... )
//   SELECT * | expr , ... FROM name [WHERE expr]
//          [ORDER BY expr [ASC|DESC] , ...] [LIMIT n [OFFSET m]]
//   UPDATE name SET column = expr , ... [WHERE expr]
//   DELETE FROM name [WHERE expr]
//   BEGIN [TRANSACTION]   COMMIT / END [TRANSACTION]   ROLLBACK [TRANSACTION]
//
//   type:  INTEGER / INT, TEXT / VARCHAR / CHAR, REAL / FLOAT / DOUBLE, BLOB
//   value: 42, -3.5, 'text', x'0AFF', NULL
//   expr:  values, columns, rowid, ( ), + - * / %, = != <> < <= > >=,
//          AND OR NOT, IS [NOT] NULL, COUNT(*) COUNT(x) SUM MIN MAX AVG
class Parser {
public:
    explicit Parser(const std::string& sql);

    Statement parse();

private:
    Statement parse_create();
    Statement parse_drop();
    Statement parse_insert();
    Statement parse_select();
    Statement parse_update();
    Statement parse_delete();
    Statement parse_transaction();

    void parse_where(Statement& statement);
    // A whole number >= 0, for LIMIT / OFFSET
    std::int64_t parse_count(const char* clause);

    Column parse_column();
    Value parse_value();

    std::unique_ptr<Expr> parse_expr();
    std::unique_ptr<Expr> parse_or();
    std::unique_ptr<Expr> parse_and();
    std::unique_ptr<Expr> parse_not();
    std::unique_ptr<Expr> parse_equality();
    std::unique_ptr<Expr> parse_relational();
    std::unique_ptr<Expr> parse_additive();
    std::unique_ptr<Expr> parse_multiplicative();
    std::unique_ptr<Expr> parse_unary();
    std::unique_ptr<Expr> parse_primary();

    const Token& peek() const;
    Token next();
    bool is_keyword(const char* keyword) const;
    void expect_keyword(const char* keyword);
    void expect_symbol(const char* symbol);
    bool accept_symbol(const char* symbol);
    std::string expect_name(const char* what);
    // Only an optional ';' may follow
    void expect_end();

    std::string sql;
    std::vector<Token> tokens;
    std::size_t pos = 0;
};

#endif //PARSER_H
