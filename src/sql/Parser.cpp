#include "Parser.h"
#include "ExceptionHandler.h"

#include <cctype>

static std::string to_upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

Parser::Parser(const std::string& sql)
    : sql(sql) {
    tokens = Tokenizer(this->sql).tokenize();
}

Statement Parser::parse() {
    if (is_keyword("CREATE")) return parse_create();
    if (is_keyword("DROP")) return parse_drop();
    if (is_keyword("INSERT")) return parse_insert();
    if (is_keyword("SELECT")) return parse_select();
    if (is_keyword("UPDATE")) return parse_update();
    if (is_keyword("DELETE")) return parse_delete();
    if (is_keyword("BEGIN") || is_keyword("COMMIT") || is_keyword("END") || is_keyword("ROLLBACK")) {
        return parse_transaction();
    }

    throw UnrecognizedStatementException(sql);
}

Statement Parser::parse_create() {
    expect_keyword("CREATE");
    expect_keyword("TABLE");

    Statement statement{};
    statement.type = StatementType::CreateTable;
    statement.table_name = expect_name("table name");
    statement.schema.name = statement.table_name;

    expect_symbol("(");
    do {
        Column column = parse_column();

        for (const Column& other : statement.schema.columns) {
            if (to_upper(other.name) == to_upper(column.name)) {
                throw DuplicateColumnException(column.name);
            }
        }
        if (column.primary_key && statement.schema.primary_key_index() >= 0) {
            throw SyntaxErrorException("table has more than one primary key");
        }

        statement.schema.columns.push_back(column);
    } while (accept_symbol(","));
    expect_symbol(")");
    expect_end();

    // The text is what gets saved in the schema, without the ';'
    std::string text = sql;
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) || text.back() == ';')) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    statement.schema.sql = text.substr(start);

    return statement;
}

Column Parser::parse_column() {
    Column column;
    column.name = expect_name("column name");

    std::string type = to_upper(expect_name("column type"));
    if (type == "INTEGER" || type == "INT") {
        column.type = DataType::Integer;
    } else if (type == "TEXT" || type == "VARCHAR" || type == "CHAR") {
        column.type = DataType::Text;
    } else if (type == "REAL" || type == "FLOAT" || type == "DOUBLE") {
        column.type = DataType::Real;
    } else if (type == "BLOB") {
        column.type = DataType::Blob;
    } else {
        throw SyntaxErrorException("unknown type " + type);
    }

    if (is_keyword("PRIMARY")) {
        next();
        expect_keyword("KEY");
        // Other primary keys would need an index, which we don't have
        if (column.type != DataType::Integer) {
            throw SyntaxErrorException("only INTEGER PRIMARY KEY is supported");
        }
        column.primary_key = true;
    }

    return column;
}

Statement Parser::parse_drop() {
    expect_keyword("DROP");
    expect_keyword("TABLE");

    Statement statement{};
    statement.type = StatementType::DropTable;
    statement.table_name = expect_name("table name");
    expect_end();
    return statement;
}

Statement Parser::parse_insert() {
    expect_keyword("INSERT");
    expect_keyword("INTO");

    Statement statement{};
    statement.type = StatementType::Insert;
    statement.table_name = expect_name("table name");
    expect_keyword("VALUES");

    expect_symbol("(");
    do {
        statement.values.push_back(parse_value());
    } while (accept_symbol(","));
    expect_symbol(")");
    expect_end();
    return statement;
}

Statement Parser::parse_select() {
    expect_keyword("SELECT");

    Statement statement{};
    statement.type = StatementType::Select;
    do {
        SelectItem item;
        if (accept_symbol("*")) {
            item.star = true;
        } else {
            item.expr = parse_expr();
        }
        statement.select_items.push_back(std::move(item));
    } while (accept_symbol(","));

    expect_keyword("FROM");
    statement.table_name = expect_name("table name");
    parse_where(statement);

    if (is_keyword("ORDER")) {
        next();
        expect_keyword("BY");
        do {
            OrderTerm term;
            term.expr = parse_expr();
            if (is_keyword("DESC")) {
                next();
                term.descending = true;
            } else if (is_keyword("ASC")) {
                next();
            }
            statement.order_by.push_back(std::move(term));
        } while (accept_symbol(","));
    }

    if (is_keyword("LIMIT")) {
        next();
        statement.limit = parse_count("LIMIT");
        if (is_keyword("OFFSET")) {
            next();
            statement.offset = parse_count("OFFSET");
        }
    }

    expect_end();
    return statement;
}

Statement Parser::parse_update() {
    expect_keyword("UPDATE");

    Statement statement{};
    statement.type = StatementType::Update;
    statement.table_name = expect_name("table name");

    expect_keyword("SET");
    do {
        Assignment assignment;
        assignment.column = expect_name("column name");
        expect_symbol("=");
        assignment.expr = parse_expr();
        statement.assignments.push_back(std::move(assignment));
    } while (accept_symbol(","));

    parse_where(statement);
    expect_end();
    return statement;
}

Statement Parser::parse_delete() {
    expect_keyword("DELETE");
    expect_keyword("FROM");

    Statement statement{};
    statement.type = StatementType::Delete;
    statement.table_name = expect_name("table name");
    parse_where(statement);
    expect_end();
    return statement;
}

Statement Parser::parse_transaction() {
    Statement statement{};
    std::string keyword = to_upper(next().text);
    if (keyword == "BEGIN") {
        statement.type = StatementType::Begin;
    } else if (keyword == "ROLLBACK") {
        statement.type = StatementType::Rollback;
    } else {
        statement.type = StatementType::Commit;     // COMMIT or END
    }

    if (is_keyword("TRANSACTION")) {
        next();
    }
    expect_end();
    return statement;
}

void Parser::parse_where(Statement& statement) {
    if (is_keyword("WHERE")) {
        next();
        statement.where = parse_expr();
    }
}

std::int64_t Parser::parse_count(const char* clause) {
    Value value = parse_value();
    if (value.get_type() != DataType::Integer || value.as_integer() < 0) {
        throw SyntaxErrorException(std::string(clause) + " needs a whole number >= 0");
    }
    return value.as_integer();
}

// ---- expressions, from the weakest operator to the strongest ----
//   OR, AND, NOT, then = != IS, then < <= > >=, then + -, then * / %, then unary - +

static std::unique_ptr<Expr> make_binary(const std::string& op, std::unique_ptr<Expr> left,
                                         std::unique_ptr<Expr> right) {
    auto expr = std::make_unique<Expr>();
    expr->kind = ExprKind::Binary;
    expr->op = op;
    expr->args.push_back(std::move(left));
    expr->args.push_back(std::move(right));
    return expr;
}

std::unique_ptr<Expr> Parser::parse_expr() {
    return parse_or();
}

std::unique_ptr<Expr> Parser::parse_or() {
    std::unique_ptr<Expr> left = parse_and();
    while (is_keyword("OR")) {
        next();
        left = make_binary("OR", std::move(left), parse_and());
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_and() {
    std::unique_ptr<Expr> left = parse_not();
    while (is_keyword("AND")) {
        next();
        left = make_binary("AND", std::move(left), parse_not());
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_not() {
    if (is_keyword("NOT")) {
        next();
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Unary;
        expr->op = "NOT";
        expr->args.push_back(parse_not());
        return expr;
    }
    return parse_equality();
}

// = != IS bind weaker than < <= > >=, like in SQLite: a = b < c is a = (b < c)
std::unique_ptr<Expr> Parser::parse_equality() {
    std::unique_ptr<Expr> left = parse_relational();

    while (true) {
        if (is_keyword("IS")) {
            next();
            bool negated = false;
            if (is_keyword("NOT")) {
                next();
                negated = true;
            }
            expect_keyword("NULL");
            auto expr = std::make_unique<Expr>();
            expr->kind = ExprKind::IsNull;
            expr->op = negated ? "IS NOT NULL" : "IS NULL";
            expr->args.push_back(std::move(left));
            left = std::move(expr);
            continue;
        }

        const Token& token = peek();
        if (token.type != TokenType::Symbol || (token.text != "=" && token.text != "!=")) {
            return left;
        }
        std::string op = next().text;
        left = make_binary(op, std::move(left), parse_relational());
    }
}

std::unique_ptr<Expr> Parser::parse_relational() {
    std::unique_ptr<Expr> left = parse_additive();
    while (peek().type == TokenType::Symbol
           && (peek().text == "<" || peek().text == "<=" || peek().text == ">" || peek().text == ">=")) {
        std::string op = next().text;
        left = make_binary(op, std::move(left), parse_additive());
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_additive() {
    std::unique_ptr<Expr> left = parse_multiplicative();
    while (peek().type == TokenType::Symbol && (peek().text == "+" || peek().text == "-")) {
        std::string op = next().text;
        left = make_binary(op, std::move(left), parse_multiplicative());
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_multiplicative() {
    std::unique_ptr<Expr> left = parse_unary();
    while (peek().type == TokenType::Symbol
           && (peek().text == "*" || peek().text == "/" || peek().text == "%")) {
        std::string op = next().text;
        left = make_binary(op, std::move(left), parse_unary());
    }
    return left;
}

std::unique_ptr<Expr> Parser::parse_unary() {
    if (peek().type == TokenType::Symbol && (peek().text == "-" || peek().text == "+")) {
        // -5 is one number, so that -9223372036854775808 still fits
        if (tokens[pos + 1].type == TokenType::Number) {
            auto expr = std::make_unique<Expr>();
            expr->kind = ExprKind::Literal;
            expr->value = parse_value();
            return expr;
        }
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Unary;
        expr->op = next().text;
        expr->args.push_back(parse_unary());
        return expr;
    }
    return parse_primary();
}

std::unique_ptr<Expr> Parser::parse_primary() {
    if (accept_symbol("(")) {
        std::unique_ptr<Expr> expr = parse_expr();
        expect_symbol(")");
        return expr;
    }

    const Token& token = peek();
    auto expr = std::make_unique<Expr>();

    if (token.type == TokenType::Word && to_upper(token.text) != "NULL") {
        std::string name = next().text;
        std::string upper = to_upper(name);
        bool is_function = upper == "COUNT" || upper == "SUM" || upper == "MIN"
                           || upper == "MAX" || upper == "AVG";

        if (is_function && accept_symbol("(")) {
            expr->kind = ExprKind::Aggregate;
            expr->name = upper;
            if (upper == "COUNT" && accept_symbol("*")) {
                // COUNT(*) has no argument
            } else {
                expr->args.push_back(parse_expr());
            }
            expect_symbol(")");
            return expr;
        }

        expr->kind = ExprKind::Column;
        expr->name = name;
        return expr;
    }

    expr->kind = ExprKind::Literal;
    expr->value = parse_value();
    return expr;
}

Value Parser::parse_value() {
    // A sign in front of a number
    std::string sign;
    if (accept_symbol("-")) {
        sign = "-";
    } else {
        accept_symbol("+");
    }

    Token token = next();
    if (!sign.empty() && token.type != TokenType::Number) {
        throw SyntaxErrorException("expected a number after " + sign);
    }

    switch (token.type) {
        case TokenType::Number: {
            std::optional<Value> number = Value::parse_number(sign + token.text);
            if (!number) {
                throw SyntaxErrorException("bad number " + token.text);
            }
            return *number;
        }
        case TokenType::String:
            return Value::text(token.text);
        case TokenType::Blob: {
            const std::string& hex = token.text;
            if (hex.size() % 2 != 0) {
                throw SyntaxErrorException("blob needs an even number of hex digits");
            }
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                if (!std::isxdigit(static_cast<unsigned char>(hex[i]))
                    || !std::isxdigit(static_cast<unsigned char>(hex[i + 1]))) {
                    throw SyntaxErrorException("bad hex digit in blob");
                }
                bytes.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            }
            return Value::blob(bytes);
        }
        case TokenType::Word:
            if (to_upper(token.text) == "NULL") {
                return Value();
            }
            throw SyntaxErrorException("expected a value, got " + token.text
                                       + " (text needs quotes: '" + token.text + "')");
        default:
            throw SyntaxErrorException("expected a value");
    }
}

const Token& Parser::peek() const {
    return tokens[pos];
}

Token Parser::next() {
    Token token = tokens[pos];
    if (token.type != TokenType::End) {
        pos++;
    }
    return token;
}

bool Parser::is_keyword(const char* keyword) const {
    return peek().type == TokenType::Word && to_upper(peek().text) == keyword;
}

void Parser::expect_keyword(const char* keyword) {
    if (!is_keyword(keyword)) {
        throw SyntaxErrorException(std::string("expected ") + keyword);
    }
    next();
}

void Parser::expect_symbol(const char* symbol) {
    if (!accept_symbol(symbol)) {
        throw SyntaxErrorException(std::string("expected '") + symbol + "'");
    }
}

bool Parser::accept_symbol(const char* symbol) {
    if (peek().type == TokenType::Symbol && peek().text == symbol) {
        next();
        return true;
    }
    return false;
}

std::string Parser::expect_name(const char* what) {
    if (peek().type != TokenType::Word) {
        throw SyntaxErrorException(std::string("expected ") + what);
    }
    return next().text;
}

void Parser::expect_end() {
    accept_symbol(";");
    if (peek().type != TokenType::End) {
        throw SyntaxErrorException("unexpected '" + peek().text + "' at the end");
    }
}
