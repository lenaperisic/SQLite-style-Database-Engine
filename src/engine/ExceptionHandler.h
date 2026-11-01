#ifndef EXCEPTIONHANDLER_H
#define EXCEPTIONHANDLER_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

class ExceptionHandler : public std::exception {
private:
    std::string message;
public:
    explicit ExceptionHandler(const std::string& msg)
        : message(msg) {}
    const char* what() const noexcept override {
        return message.c_str();
    }
};

// Meta-command starting with '.' that we don't know, e.g. ".tables"
class UnrecognizedCommandException : public ExceptionHandler {
public:
    explicit UnrecognizedCommandException(const std::string& cmd)
        : ExceptionHandler("Unrecognized command '" + cmd + "'") {}
};

// SQL statement whose first word isn't a known keyword, e.g. "delete foo"
class UnrecognizedStatementException : public ExceptionHandler {
public:
    explicit UnrecognizedStatementException(const std::string& input)
        : ExceptionHandler("Unrecognized keyword at start of '" + input + "'.") {}
};

// Statement has the right keyword but the rest doesn't follow the grammar
class SyntaxErrorException : public ExceptionHandler {
public:
    SyntaxErrorException()
        : ExceptionHandler("Syntax error. Could not parse statement.") {}
    explicit SyntaxErrorException(const std::string& detail)
        : ExceptionHandler("Syntax error: " + detail + ".") {}
};

class TableExistsException : public ExceptionHandler {
public:
    explicit TableExistsException(const std::string& table)
        : ExceptionHandler("Error: table " + table + " already exists.") {}
};

class TableNotFoundException : public ExceptionHandler {
public:
    explicit TableNotFoundException(const std::string& table)
        : ExceptionHandler("Error: no such table: " + table) {}
};

class DuplicateColumnException : public ExceptionHandler {
public:
    explicit DuplicateColumnException(const std::string& column)
        : ExceptionHandler("Error: duplicate column name: " + column) {}
};

// INSERT gave a different number of values than the table has columns
class ColumnCountException : public ExceptionHandler {
public:
    ColumnCountException(const std::string& table, std::size_t columns, std::size_t values)
        : ExceptionHandler("Error: table " + table + " has " + std::to_string(columns)
                           + " columns but " + std::to_string(values) + " values were supplied.") {}
};

// The INTEGER PRIMARY KEY column got a value that isn't a whole number
class DatatypeMismatchException : public ExceptionHandler {
public:
    explicit DatatypeMismatchException(const std::string& column)
        : ExceptionHandler("Error: datatype mismatch: " + column + " must be an integer.") {}
};

// Row ids are stored as 32-bit keys
class IdTooLargeException : public ExceptionHandler {
public:
    IdTooLargeException()
        : ExceptionHandler("Error: ID is too large (max 4294967295).") {}
};

// The row's record doesn't fit in a leaf cell's slot
class RowTooLargeException : public ExceptionHandler {
public:
    RowTooLargeException(std::size_t size, std::size_t max_size)
        : ExceptionHandler("Error: Row is too large (" + std::to_string(size)
                           + " bytes, max " + std::to_string(max_size) + ").") {}
};

class NegativeIdException : public ExceptionHandler {
public:
    NegativeIdException()
        : ExceptionHandler("ID must be positive.") {}
};

class DuplicateKeyException : public ExceptionHandler {
public:
    DuplicateKeyException()
        : ExceptionHandler("Error: Duplicate key.") {}
};

class RowNotFoundException : public ExceptionHandler {
public:
    explicit RowNotFoundException(std::uint32_t id)
        : ExceptionHandler("Error: No row with id " + std::to_string(id) + ".") {}
};

// BEGIN inside a transaction, COMMIT / ROLLBACK outside one
class TransactionException : public ExceptionHandler {
public:
    explicit TransactionException(const std::string& msg)
        : ExceptionHandler("Error: " + msg + ".") {}
};

class NoSuchColumnException : public ExceptionHandler {
public:
    explicit NoSuchColumnException(const std::string& column)
        : ExceptionHandler("Error: no such column: " + column) {}
};

// Problem opening, reading or writing the database file
class DatabaseFileException : public ExceptionHandler {
public:
    explicit DatabaseFileException(const std::string& msg)
        : ExceptionHandler("Error: " + msg) {}
};

#endif //EXCEPTIONHANDLER_H
