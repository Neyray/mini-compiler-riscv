#include "Lexer.h"
#include <cctype>
#include <unordered_map>

Lexer::Lexer(const std::string& source) : src(source), pos(0), line(1), col(1) {}

char Lexer::peek() const { return pos < src.size() ? src[pos] : '\0'; }

char Lexer::peekNext() const { return pos + 1 < src.size() ? src[pos + 1] : '\0'; }

char Lexer::get() {
    char c = peek();
    if (c != '\0') {
        pos++;
        if (c == '\n') { line++; col = 1; }
        else { col++; }
    }
    return c;
}

void Lexer::skipWhitespace() {
    while (peek() != '\0' && isspace(static_cast<unsigned char>(peek()))) get();
}

bool Lexer::skipComment() {
    if (peek() == '/' && peekNext() == '/') {
        get(); get();
        while (peek() != '\n' && peek() != '\0') get();
        return true;
    }
    if (peek() == '/' && peekNext() == '*') {
        get(); get();
        while (peek() != '\0') {
            if (peek() == '*' && peekNext() == '/') { get(); get(); break; }
            get();
        }
        return true;
    }
    return false;
}

Token Lexer::identifierOrKeyword() {
    static std::unordered_map<std::string, TokenType> keywords = {
        {"int", TokenType::KW_INT}, {"void", TokenType::KW_VOID},
        {"const", TokenType::KW_CONST}, {"if", TokenType::KW_IF},
        {"else", TokenType::KW_ELSE}, {"while", TokenType::KW_WHILE},
        {"break", TokenType::KW_BREAK}, {"continue", TokenType::KW_CONTINUE},
        {"return", TokenType::KW_RETURN}
    };
    std::string val;
    while (isalnum(static_cast<unsigned char>(peek())) || peek() == '_') val += get();
    auto it = keywords.find(val);
    return it != keywords.end() ? makeToken(it->second, val) : makeToken(TokenType::IDENTIFIER, val);
}

Token Lexer::number() {
    std::string val;
    while (isdigit(static_cast<unsigned char>(peek()))) val += get();
    return makeToken(TokenType::NUMBER, val);
}

Token Lexer::nextToken() {
    for (;;) {
        skipWhitespace();
        if (!skipComment()) break;
    }

    char c = peek();
    if (c == '\0') return makeToken(TokenType::END_OF_FILE, "EOF");

    if (isalpha(static_cast<unsigned char>(c)) || c == '_') return identifierOrKeyword();
    if (isdigit(static_cast<unsigned char>(c))) return number();

    if (c == '+') { get(); return makeToken(TokenType::PLUS, "+"); }
    if (c == '-') { get(); return makeToken(TokenType::MINUS, "-"); }
    if (c == '*') { get(); return makeToken(TokenType::STAR, "*"); }
    if (c == '/') { get(); return makeToken(TokenType::SLASH, "/"); }
    if (c == '%') { get(); return makeToken(TokenType::PERCENT, "%"); }
    if (c == '=') { get(); return peek() == '=' ? (get(), makeToken(TokenType::EQ_EQ, "==")) : makeToken(TokenType::EQ, "="); }
    if (c == '!') { get(); return peek() == '=' ? (get(), makeToken(TokenType::NEQ, "!=")) : makeToken(TokenType::NOT, "!"); }
    if (c == '<') { get(); return peek() == '=' ? (get(), makeToken(TokenType::LE, "<=")) : makeToken(TokenType::LT, "<"); }
    if (c == '>') { get(); return peek() == '=' ? (get(), makeToken(TokenType::GE, ">=")) : makeToken(TokenType::GT, ">"); }
    if (c == '&') { get(); if (peek() == '&') { get(); return makeToken(TokenType::AND_AND, "&&"); } return makeToken(TokenType::UNKNOWN, "&"); }
    if (c == '|') { get(); if (peek() == '|') { get(); return makeToken(TokenType::OR_OR, "||"); } return makeToken(TokenType::UNKNOWN, "|"); }

    if (c == ';') { get(); return makeToken(TokenType::SEMICOLON, ";"); }
    if (c == ',') { get(); return makeToken(TokenType::COMMA, ","); }
    if (c == '(') { get(); return makeToken(TokenType::LPAREN, "("); }
    if (c == ')') { get(); return makeToken(TokenType::RPAREN, ")"); }
    if (c == '{') { get(); return makeToken(TokenType::LBRACE, "{"); }
    if (c == '}') { get(); return makeToken(TokenType::RBRACE, "}"); }

    get();
    return makeToken(TokenType::UNKNOWN, std::string(1, c));
}

Token Lexer::makeToken(TokenType type, const std::string& val) {
    return {type, val, line, col};
}
