#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <string>
#include <vector>

enum class TokenType {
    Word,       // keyword or name: CREATE, users, id
    Number,     // 42, 3.5, 1e10 (a leading minus is a separate Symbol)
    String,     // 'it''s' (text holds it's, without the quotes)
    Blob,       // x'0AFF' (text holds 0AFF)
    Symbol,     // ( ) , ; * = - + / % < > <= >= != (<> and == become != and =)
    End
};

struct Token {
    TokenType type;
    std::string text;
};

// Splits one line of SQL into tokens. Throws SyntaxErrorException on a
// character it doesn't know or a string without its closing quote.
class Tokenizer {
public:
    explicit Tokenizer(const std::string& sql) : sql(sql) {}

    // The tokens, always ending with an End token
    std::vector<Token> tokenize();

private:
    Token read_word();
    Token read_number();
    Token read_string();
    Token read_blob();

    const std::string& sql;
    std::size_t pos = 0;
};

#endif //TOKENIZER_H
