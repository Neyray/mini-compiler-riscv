#pragma once

#include "AST.h"

// 语义检查通过后的 AST 包装。
// 不复制、不修改前端 AST；只表示该 AST 已经通过 SemanticAnalyzer 检查。
struct CheckedAST {
    CompUnitNode* root = nullptr;

    explicit CheckedAST(CompUnitNode* r = nullptr) : root(r) {}
};
