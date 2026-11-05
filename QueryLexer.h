#pragma once
#include <cstddef>
#include <string>
#include <vector>

enum class TokenType {
    WORD,      // keywords and field names; the parser compares them ignoring case
    NUMBER,
    STRING,
    SYMBOL,    // [ ] ( ) , ; = != < <= > >=
    END
};

struct Token {
    TokenType type;
    std::string text;  // for STRING, the contents without the quotes
    size_t line;
    size_t column;
};

struct LexResult {
    bool ok;
    std::vector<Token> tokens; // always ends with an END token when ok
    std::string error;
};

LexResult tokenize(const std::string& input);
