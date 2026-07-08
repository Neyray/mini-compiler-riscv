#pragma once
#include "Lexer.h"
#include "AST.h"
#include <memory>
#include <vector>

class Parser {
public:
    explicit Parser(Lexer& lexer);
    std::unique_ptr<CompUnitNode> parseCompUnit();

private:
    std::vector<Token> tokens;
    size_t pos;

    const Token& cur() const;
    const Token& lookahead(size_t k) const;
    bool check(TokenType type) const;
    bool accept(TokenType type);
    Token expect(TokenType type, const std::string& msg);

    std::unique_ptr<ASTNode> parseDeclOrFuncDef();
    std::unique_ptr<DeclStmtNode> parseDecl(bool isConst);
    std::unique_ptr<FuncDefNode> parseFuncDef();
    std::unique_ptr<StmtNode> parseStmt();
    std::unique_ptr<BlockNode> parseBlock();
    std::vector<std::string> parseParams();

    std::unique_ptr<ExprNode> parseExpr();
    std::unique_ptr<ExprNode> parseLOrExpr();
    std::unique_ptr<ExprNode> parseLAndExpr();
    std::unique_ptr<ExprNode> parseRelExpr();
    std::unique_ptr<ExprNode> parseAddExpr();
    std::unique_ptr<ExprNode> parseMulExpr();
    std::unique_ptr<ExprNode> parseUnaryExpr();
    std::unique_ptr<ExprNode> parsePrimaryExpr();

    std::vector<std::unique_ptr<ExprNode>> parseCallArgs();
};
