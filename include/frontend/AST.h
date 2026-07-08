#pragma once
#include <string>
#include <vector>
#include <memory>
#include <iostream>

enum class TokenType {
    KW_INT, KW_VOID, KW_CONST, KW_IF, KW_ELSE, KW_WHILE,
    KW_BREAK, KW_CONTINUE, KW_RETURN,
    IDENTIFIER, NUMBER,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, EQ_EQ, NEQ, LT, GT, LE, GE,
    AND_AND, OR_OR, NOT,
    SEMICOLON, COMMA, LPAREN, RPAREN, LBRACE, RBRACE,
    END_OF_FILE, UNKNOWN
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;
};

class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void print(int indent = 0) const = 0;
};

class ExprNode : public ASTNode {};

class LiteralNode : public ExprNode {
public:
    int value;
    explicit LiteralNode(int v) : value(v) {}
    void print(int indent) const override;
};

class VarNode : public ExprNode {
public:
    std::string name;
    explicit VarNode(std::string n) : name(std::move(n)) {}
    void print(int indent) const override;
};

class UnaryNode : public ExprNode {
public:
    std::string op;
    std::unique_ptr<ExprNode> operand;
    UnaryNode(std::string op, std::unique_ptr<ExprNode> opnd) : op(std::move(op)), operand(std::move(opnd)) {}
    void print(int indent) const override;
};

class BinaryNode : public ExprNode {
public:
    std::string op;
    std::unique_ptr<ExprNode> left, right;
    BinaryNode(std::string op, std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
        : op(std::move(op)), left(std::move(l)), right(std::move(r)) {}
    void print(int indent) const override;
};

class CallNode : public ExprNode {
public:
    std::string funcName;
    std::vector<std::unique_ptr<ExprNode>> args;
    CallNode(std::string name, std::vector<std::unique_ptr<ExprNode>> args)
        : funcName(std::move(name)), args(std::move(args)) {}
    void print(int indent) const override;
};

class StmtNode : public ASTNode {};

class BlockNode : public StmtNode {
public:
    std::vector<std::unique_ptr<StmtNode>> stmts;
    void print(int indent) const override;
};

class DeclStmtNode : public StmtNode {
public:
    bool isConst;
    std::string name;
    std::unique_ptr<ExprNode> initExpr;
    DeclStmtNode(bool isConst, std::string name, std::unique_ptr<ExprNode> init)
        : isConst(isConst), name(std::move(name)), initExpr(std::move(init)) {}
    void print(int indent) const override;
};

class AssignNode : public StmtNode {
public:
    std::string name;
    std::unique_ptr<ExprNode> rhs;
    AssignNode(std::string name, std::unique_ptr<ExprNode> rhs) : name(std::move(name)), rhs(std::move(rhs)) {}
    void print(int indent) const override;
};

class IfNode : public StmtNode {
public:
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<StmtNode> thenStmt;
    std::unique_ptr<StmtNode> elseStmt;
    IfNode(std::unique_ptr<ExprNode> cond, std::unique_ptr<StmtNode> thenS, std::unique_ptr<StmtNode> elseS)
        : condition(std::move(cond)), thenStmt(std::move(thenS)), elseStmt(std::move(elseS)) {}
    void print(int indent) const override;
};

class WhileNode : public StmtNode {
public:
    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<StmtNode> body;
    WhileNode(std::unique_ptr<ExprNode> cond, std::unique_ptr<StmtNode> body)
        : condition(std::move(cond)), body(std::move(body)) {}
    void print(int indent) const override;
};

class BreakNode : public StmtNode {
public:
    void print(int indent) const override;
};

class ContinueNode : public StmtNode {
public:
    void print(int indent) const override;
};

class ReturnNode : public StmtNode {
public:
    std::unique_ptr<ExprNode> retExpr;
    explicit ReturnNode(std::unique_ptr<ExprNode> expr) : retExpr(std::move(expr)) {}
    void print(int indent) const override;
};

class ExprStmtNode : public StmtNode {
public:
    std::unique_ptr<ExprNode> expr;
    explicit ExprStmtNode(std::unique_ptr<ExprNode> e) : expr(std::move(e)) {}
    void print(int indent) const override;
};

class FuncDefNode : public ASTNode {
public:
    std::string returnType;
    std::string name;
    std::vector<std::string> params;
    std::unique_ptr<BlockNode> body;
    FuncDefNode(std::string ret, std::string name, std::vector<std::string> params, std::unique_ptr<BlockNode> body)
        : returnType(std::move(ret)), name(std::move(name)), params(std::move(params)), body(std::move(body)) {}
    void print(int indent) const override;
};

class CompUnitNode : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> items;
    void print(int indent) const override;
};
