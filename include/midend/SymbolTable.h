#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

// ToyC 中端符号信息。
// 适配前端 AST.h：DeclStmtNode / FuncDefNode / ExprNode / StmtNode。

enum class ValueType {
    Int,
    Void
};

enum class SymbolKind {
    Variable,
    Constant,
    Function,
    Parameter
};

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    ValueType type = ValueType::Int;

    bool isConst = false;
    bool isGlobal = false;
    bool hasConstValue = false;
    int constValue = 0;

    // 函数专用
    ValueType returnType = ValueType::Void;
    std::vector<ValueType> paramTypes;
};

class SymbolTable {
private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes;

public:
    void enterScope();
    void exitScope();

    bool isGlobalScope() const;
    int depth() const;

    bool currentScopeContains(const std::string& name) const;
    bool declare(const Symbol& sym);

    Symbol* lookup(const std::string& name);
    const Symbol* lookup(const std::string& name) const;
};
