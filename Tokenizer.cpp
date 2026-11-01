#include "Tokenizer.h"
#include "ExceptionHandler.h"

#include <cctype>

static bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

static bool is_word_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

static bool is_word_char(char c) {
    return is_word_start(c) || is_digit(c);
}

static bool is_two_char_operator(const std::string& text) {
    return text == "<=" || text == ">=" || text == "!=" || text == "<>" || text == "==";
}

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) {
            pos++;
        }
        if (pos >= sql.size()) {
            tokens.push_back({TokenType::End, ""});
            return tokens;
        }

        char c = sql[pos];
        bool next_is_quote = pos + 1 < sql.size() && sql[pos + 1] == '\'';

        if ((c == 'x' || c == 'X') && next_is_quote) {
            tokens.push_back(read_blob());
        } else if (is_word_start(c)) {
            tokens.push_back(read_word());
        } else if (is_digit(c) || (c == '.' && pos + 1 < sql.size() && is_digit(sql[pos + 1]))) {
            tokens.push_back(read_number());
        } else if (c == '\'') {
            tokens.push_back(read_string());
        } else if (pos + 1 < sql.size() && is_two_char_operator(sql.substr(pos, 2))) {
            // == means =, <> means !=
            std::string op = sql.substr(pos, 2);
            tokens.push_back({TokenType::Symbol, op == "==" ? "=" : (op == "<>" ? "!=" : op)});
            pos += 2;
        } else if (std::string("(),;*=-+<>/%").find(c) != std::string::npos) {
            tokens.push_back({TokenType::Symbol, std::string(1, c)});
            pos++;
        } else {
            throw SyntaxErrorException("unexpected character '" + std::string(1, c) + "'");
        }
    }
}

Token Tokenizer::read_word() {
    std::size_t start = pos;
    while (pos < sql.size() && is_word_char(sql[pos])) {
        pos++;
    }
    return {TokenType::Word, sql.substr(start, pos - start)};
}

Token Tokenizer::read_number() {
    std::size_t start = pos;
    while (pos < sql.size()) {
        char c = sql[pos];
        bool exponent_sign = (c == '-' || c == '+') && pos > start
                             && (sql[pos - 1] == 'e' || sql[pos - 1] == 'E');
        if (!is_digit(c) && c != '.' && c != 'e' && c != 'E' && !exponent_sign) {
            break;
        }
        pos++;
    }
    // A number glued to a name, like 12abc, isn't valid
    if (pos < sql.size() && is_word_char(sql[pos])) {
        throw SyntaxErrorException("bad number '" + sql.substr(start, pos - start + 1) + "...'");
    }
    return {TokenType::Number, sql.substr(start, pos - start)};
}

// 'it''s' -> it's  (two quotes in a row mean one quote)
Token Tokenizer::read_string() {
    std::string text;
    pos++;  // opening quote
    while (true) {
        if (pos >= sql.size()) {
            throw SyntaxErrorException("unterminated string");
        }
        if (sql[pos] == '\'') {
            if (pos + 1 < sql.size() && sql[pos + 1] == '\'') {
                text += '\'';
                pos += 2;
                continue;
            }
            pos++;
            return {TokenType::String, text};
        }
        text += sql[pos++];
    }
}

Token Tokenizer::read_blob() {
    std::size_t end = sql.find('\'', pos + 2);
    if (end == std::string::npos) {
        throw SyntaxErrorException("unterminated blob");
    }
    std::string hex = sql.substr(pos + 2, end - pos - 2);
    pos = end + 1;
    return {TokenType::Blob, hex};
}
