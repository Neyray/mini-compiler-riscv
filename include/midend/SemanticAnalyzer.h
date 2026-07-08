#pragma once

#include <string>
#include <stdexcept>
#include "AST.h"
#include "SymbolTable.h"
#include "CheckedAST.h"

class SemanticError : public std::runtime_error {
public:
    explicit SemanticError(const std::string& msg)
        : std::runtime_error("Semantic error: " + msg) {}
};

class SemanticAnalyzer {
private:
    SymbolTable symbols;
    ValueType currentReturnType = ValueType::Void;
    int loopDepth = 0;

private:
    void checkCompUnit(CompUnitNode* node);
    void checkTopLevelItem(ASTNode* node);
    void checkGlobalDecl(DeclStmtNode* node);
    void checkFuncDef(FuncDefNode* node);

    void checkBlock(BlockNode* node, bool createScope);
    void checkStmt(StmtNode* node);
    ValueType checkExpr(ExprNode* node);

    bool stmtAlwaysReturns(StmtNode* node);
    bool blockAlwaysReturns(BlockNode* node);

    int evalConstExpr(ExprNode* node);
    ValueType stringToType(const std::string& s) const;
    std::string typeToString(ValueType t) const;

public:
    CheckedAST check(CompUnitNode* root);
};
