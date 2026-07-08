#pragma once
#include "AST.h"
#include <string>

class Lexer {
public:
    explicit Lexer(const std::string& source);
    Token nextToken();

private:
    std::string src;
    size_t pos;
    int line, col;

    char peek() const;
    char peekNext() const;
    char get();
    void skipWhitespace();
    bool skipComment();
    Token makeToken(TokenType type, const std::string& val);
    Token identifierOrKeyword();
    Token number();
};
