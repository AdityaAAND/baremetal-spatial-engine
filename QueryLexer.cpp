#include "QueryLexer.h"
#include <cctype>

static bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
static bool is_word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
static bool is_word_char(char c) { return is_word_start(c) || is_digit(c); }

namespace {

class Lexer {
private:
    const std::string& input;
    size_t pos = 0;
    size_t line = 1;
    size_t column = 1;

    char peek(size_t ahead = 0) const {
        return pos + ahead < input.size() ? input[pos + ahead] : '\0';
    }

    char advance() {
        const char c = input[pos++];
        if (c == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        return c;
    }

    void read_digits(std::string& text) {
        while (is_digit(peek())) text += advance();
    }

    std::string where(size_t at_line, size_t at_column) const {
        return "line " + std::to_string(at_line) + ", column " + std::to_string(at_column) + ": ";
    }

    // The sign belongs to the number because the language has no arithmetic, so "-" is never an operator
    bool read_number(Token& token, std::string& error) {
        if (peek() == '-' || peek() == '+') token.text += advance();
        read_digits(token.text);
        if (peek() == '.') {
            token.text += advance();
            read_digits(token.text);
        }
        if (peek() == 'e' || peek() == 'E') {
            const size_t sign = (peek(1) == '-' || peek(1) == '+') ? 1 : 0;
            if (is_digit(peek(1 + sign))) {
                token.text += advance();
                if (sign) token.text += advance();
                read_digits(token.text);
            }
        }
        if (is_word_char(peek())) {
            error = where(line, column) + "unexpected '" + std::string(1, peek()) + "' after number";
            return false;
        }
        return true;
    }

    bool read_string(Token& token, std::string& error) {
        const char quote = advance();
        while (pos < input.size() && peek() != quote && peek() != '\n') token.text += advance();
        if (peek() != quote) {
            error = where(token.line, token.column) + "string has no closing quote";
            return false;
        }
        advance();
        return true;
    }

public:
    explicit Lexer(const std::string& text) : input(text) {}

    LexResult run() {
        LexResult result{};
        while (true) {
            while (std::isspace(static_cast<unsigned char>(peek())) != 0) advance();

            Token token{TokenType::END, "", line, column};
            if (pos >= input.size()) {
                result.tokens.push_back(token);
                result.ok = true;
                return result;
            }

            const char c = peek();
            if (is_word_start(c)) {
                token.type = TokenType::WORD;
                while (is_word_char(peek())) token.text += advance();
            } else if (is_digit(c) || ((c == '-' || c == '+' || c == '.') && (is_digit(peek(1)) || (c != '.' && peek(1) == '.' && is_digit(peek(2)))))) {
                token.type = TokenType::NUMBER;
                if (!read_number(token, result.error)) return result;
            } else if (c == '\'' || c == '"') {
                token.type = TokenType::STRING;
                if (!read_string(token, result.error)) return result;
            } else if ((c == '<' || c == '>' || c == '!') && peek(1) == '=') {
                token.type = TokenType::SYMBOL;
                token.text += advance();
                token.text += advance();
            } else if (std::string("[](),;=<>").find(c) != std::string::npos) {
                token.type = TokenType::SYMBOL;
                token.text += advance();
            } else {
                result.error = where(line, column) + "unexpected character '" + std::string(1, c) + "'";
                return result;
            }
            result.tokens.push_back(token);
        }
    }
};

} // namespace

LexResult tokenize(const std::string& input) {
    return Lexer(input).run();
}
