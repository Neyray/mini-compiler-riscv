#include "Parser.h"
#include <stdexcept>

Parser::Parser(Lexer& l) : pos(0) {
    for (;;) {
        Token t = l.nextToken();
        tokens.push_back(t);
        if (t.type == TokenType::END_OF_FILE) break;
    }
}

const Token& Parser::cur() const {
    return tokens[pos];
}

const Token& Parser::lookahead(size_t k) const {
    size_t i = pos + k;
    if (i >= tokens.size()) i = tokens.size() - 1;
    return tokens[i];
}

bool Parser::check(TokenType type) const {
    return cur().type == type;
}

bool Parser::accept(TokenType type) {
    if (check(type)) {
        pos++;
        return true;
    }
    return false;
}

Token Parser::expect(TokenType type, const std::string& msg) {
    if (!check(type)) {
        throw std::runtime_error("Parse error at line " + std::to_string(cur().line) +
                                 ": " + msg + ", found '" + cur().value + "'");
    }
    Token tk = cur();
    pos++;
    return tk;
}

std::unique_ptr<CompUnitNode> Parser::parseCompUnit() {
    auto compUnit = std::make_unique<CompUnitNode>();
    while (!check(TokenType::END_OF_FILE)) {
        compUnit->items.push_back(parseDeclOrFuncDef());
    }
    return compUnit;
}

std::unique_ptr<ASTNode> Parser::parseDeclOrFuncDef() {
    if (check(TokenType::KW_CONST)) {
        return parseDecl(true);
    }
    if (check(TokenType::KW_VOID)) {
        return parseFuncDef();
    }
    if (check(TokenType::KW_INT)) {
        // "int" ID "(" 是函数定义；"int" ID "=" 是全局变量声明。
        if (lookahead(1).type == TokenType::IDENTIFIER &&
            lookahead(2).type == TokenType::LPAREN) {
            return parseFuncDef();
        }
        return parseDecl(false);
    }
    throw std::runtime_error("Parse error at line " + std::to_string(cur().line) +
                             ": expected const, int or void at global scope");
}

std::unique_ptr<DeclStmtNode> Parser::parseDecl(bool isConst) {
    if (isConst) {
        expect(TokenType::KW_CONST, "expect const");
    }
    expect(TokenType::KW_INT, "expect int");
    Token name = expect(TokenType::IDENTIFIER, "expect identifier");
    expect(TokenType::EQ, "expect =");
    auto init = parseExpr();
    expect(TokenType::SEMICOLON, "expect ;");
    return std::make_unique<DeclStmtNode>(isConst, name.value, std::move(init));
}

std::unique_ptr<FuncDefNode> Parser::parseFuncDef() {
    std::string retType;
    if (accept(TokenType::KW_INT)) {
        retType = "int";
    } else if (accept(TokenType::KW_VOID)) {
        retType = "void";
    } else {
        throw std::runtime_error("Expected int or void for function return type");
    }
    Token name = expect(TokenType::IDENTIFIER, "expect function name");
    expect(TokenType::LPAREN, "expect (");
    std::vector<std::string> params;
    if (!check(TokenType::RPAREN)) {
        params = parseParams();
    }
    expect(TokenType::RPAREN, "expect )");
    auto body = parseBlock();
    return std::make_unique<FuncDefNode>(retType, name.value, std::move(params), std::move(body));
}

std::vector<std::string> Parser::parseParams() {
    std::vector<std::string> params;
    for (;;) {
        expect(TokenType::KW_INT, "expect int in parameter");
        Token name = expect(TokenType::IDENTIFIER, "expect parameter name");
        params.push_back(name.value);
        if (!accept(TokenType::COMMA)) break;
    }
    return params;
}

std::unique_ptr<StmtNode> Parser::parseStmt() {
    if (check(TokenType::LBRACE)) {
        return parseBlock();
    }
    if (accept(TokenType::SEMICOLON)) {
        return std::make_unique<BlockNode>();
    }
    if (accept(TokenType::KW_IF)) {
        expect(TokenType::LPAREN, "expect ( after if");
        auto cond = parseExpr();
        expect(TokenType::RPAREN, "expect ) after condition");
        auto thenStmt = parseStmt();
        std::unique_ptr<StmtNode> elseStmt = nullptr;
        if (accept(TokenType::KW_ELSE)) {
            elseStmt = parseStmt();
        }
        return std::make_unique<IfNode>(std::move(cond), std::move(thenStmt), std::move(elseStmt));
    }
    if (accept(TokenType::KW_WHILE)) {
        expect(TokenType::LPAREN, "expect ( after while");
        auto cond = parseExpr();
        expect(TokenType::RPAREN, "expect ) after condition");
        auto body = parseStmt();
        return std::make_unique<WhileNode>(std::move(cond), std::move(body));
    }
    if (accept(TokenType::KW_BREAK)) {
        expect(TokenType::SEMICOLON, "expect ; after break");
        return std::make_unique<BreakNode>();
    }
    if (accept(TokenType::KW_CONTINUE)) {
        expect(TokenType::SEMICOLON, "expect ; after continue");
        return std::make_unique<ContinueNode>();
    }
    if (accept(TokenType::KW_RETURN)) {
        std::unique_ptr<ExprNode> retExpr = nullptr;
        if (!check(TokenType::SEMICOLON)) {
            retExpr = parseExpr();
        }
        expect(TokenType::SEMICOLON, "expect ; after return");
        return std::make_unique<ReturnNode>(std::move(retExpr));
    }
    if (check(TokenType::KW_CONST) || check(TokenType::KW_INT)) {
        return parseDecl(check(TokenType::KW_CONST));
    }
    if (check(TokenType::IDENTIFIER) && lookahead(1).type == TokenType::EQ) {
        Token name = expect(TokenType::IDENTIFIER, "expect identifier");
        expect(TokenType::EQ, "expect =");
        auto rhs = parseExpr();
        expect(TokenType::SEMICOLON, "expect ; after assignment");
        return std::make_unique<AssignNode>(name.value, std::move(rhs));
    }
    auto expr = parseExpr();
    expect(TokenType::SEMICOLON, "expect ; after expression statement");
    return std::make_unique<ExprStmtNode>(std::move(expr));
}

std::unique_ptr<BlockNode> Parser::parseBlock() {
    expect(TokenType::LBRACE, "expect {");
    auto block = std::make_unique<BlockNode>();
    while (!check(TokenType::RBRACE) && !check(TokenType::END_OF_FILE)) {
        block->stmts.push_back(parseStmt());
    }
    expect(TokenType::RBRACE, "expect }");
    return block;
}

std::unique_ptr<ExprNode> Parser::parseExpr() {
    return parseLOrExpr();
}

std::unique_ptr<ExprNode> Parser::parseLOrExpr() {
    auto expr = parseLAndExpr();
    while (check(TokenType::OR_OR)) {
        pos++;
        auto right = parseLAndExpr();
        expr = std::make_unique<BinaryNode>("||", std::move(expr), std::move(right));
    }
    return expr;
}

std::unique_ptr<ExprNode> Parser::parseLAndExpr() {
    auto expr = parseRelExpr();
    while (check(TokenType::AND_AND)) {
        pos++;
        auto right = parseRelExpr();
        expr = std::make_unique<BinaryNode>("&&", std::move(expr), std::move(right));
    }
    return expr;
}

std::unique_ptr<ExprNode> Parser::parseRelExpr() {
    auto expr = parseAddExpr();
    while (check(TokenType::LT) || check(TokenType::GT) ||
           check(TokenType::LE) || check(TokenType::GE) ||
           check(TokenType::EQ_EQ) || check(TokenType::NEQ)) {
        std::string op = cur().value;
        pos++;
        auto right = parseAddExpr();
        expr = std::make_unique<BinaryNode>(op, std::move(expr), std::move(right));
    }
    return expr;
}

std::unique_ptr<ExprNode> Parser::parseAddExpr() {
    auto expr = parseMulExpr();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        std::string op = cur().value;
        pos++;
        auto right = parseMulExpr();
        expr = std::make_unique<BinaryNode>(op, std::move(expr), std::move(right));
    }
    return expr;
}

std::unique_ptr<ExprNode> Parser::parseMulExpr() {
    auto expr = parseUnaryExpr();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        std::string op = cur().value;
        pos++;
        auto right = parseUnaryExpr();
        expr = std::make_unique<BinaryNode>(op, std::move(expr), std::move(right));
    }
    return expr;
}

std::unique_ptr<ExprNode> Parser::parseUnaryExpr() {
    if (check(TokenType::PLUS) || check(TokenType::MINUS) || check(TokenType::NOT)) {
        std::string op = cur().value;
        pos++;
        auto operand = parseUnaryExpr();
        return std::make_unique<UnaryNode>(op, std::move(operand));
    }
    return parsePrimaryExpr();
}

std::unique_ptr<ExprNode> Parser::parsePrimaryExpr() {
    if (check(TokenType::NUMBER)) {
        Token tk = cur();
        pos++;
        // 用 long long 解析再截断到 32 位，避免 -2147483648 这类边界值溢出。
        int v = static_cast<int>(std::stoll(tk.value));
        return std::make_unique<LiteralNode>(v);
    }
    if (check(TokenType::IDENTIFIER)) {
        Token name = cur();
        pos++;
        if (accept(TokenType::LPAREN)) {
            std::vector<std::unique_ptr<ExprNode>> args;
            if (!check(TokenType::RPAREN)) {
                args = parseCallArgs();
            }
            expect(TokenType::RPAREN, "expect ) after function call");
            return std::make_unique<CallNode>(name.value, std::move(args));
        }
        return std::make_unique<VarNode>(name.value);
    }
    if (accept(TokenType::LPAREN)) {
        auto expr = parseExpr();
        expect(TokenType::RPAREN, "expect )");
        return expr;
    }
    throw std::runtime_error("Parse error at line " + std::to_string(cur().line) +
                             ": unexpected token '" + cur().value + "' in expression");
}

std::vector<std::unique_ptr<ExprNode>> Parser::parseCallArgs() {
    std::vector<std::unique_ptr<ExprNode>> args;
    for (;;) {
        args.push_back(parseExpr());
        if (!accept(TokenType::COMMA)) break;
    }
    return args;
}
