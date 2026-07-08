#include "AST.h"
#include <iostream>

void LiteralNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Literal: " << value << std::endl;
}

void VarNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Var: " << name << std::endl;
}

void UnaryNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "UnaryOp: " << op << std::endl;
    operand->print(indent + 1);
}

void BinaryNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "BinaryOp: " << op << std::endl;
    left->print(indent + 1);
    right->print(indent + 1);
}

void CallNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Call: " << funcName << " (" << args.size() << " args)" << std::endl;
    for (const auto& arg : args) {
        arg->print(indent + 1);
    }
}

void BlockNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Block {" << std::endl;
    for (const auto& stmt : stmts) {
        stmt->print(indent + 1);
    }
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "}" << std::endl;
}

void DeclStmtNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << (isConst ? "ConstDecl" : "VarDecl") << ": " << name << std::endl;
    if (initExpr) {
        initExpr->print(indent + 1);
    }
}

void AssignNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Assign: " << name << " =" << std::endl;
    if (rhs) rhs->print(indent + 1);
}

void IfNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "If" << std::endl;
    condition->print(indent + 1);
    thenStmt->print(indent + 1);
    if (elseStmt) {
        for (int i = 0; i < indent; ++i) std::cout << "  ";
        std::cout << "Else" << std::endl;
        elseStmt->print(indent + 1);
    }
}

void WhileNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "While" << std::endl;
    condition->print(indent + 1);
    body->print(indent + 1);
}

void BreakNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Break" << std::endl;
}

void ContinueNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Continue" << std::endl;
}

void ReturnNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "Return" << std::endl;
    if (retExpr) retExpr->print(indent + 1);
}

void ExprStmtNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "ExprStmt" << std::endl;
    if (expr) expr->print(indent + 1);
}

void FuncDefNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "FuncDef: " << returnType << " " << name << "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << "int " << params[i];
    }
    std::cout << ")" << std::endl;
    if (body) body->print(indent + 1);
}

void CompUnitNode::print(int indent) const {
    for (int i = 0; i < indent; ++i) std::cout << "  ";
    std::cout << "CompUnit" << std::endl;
    for (const auto& item : items) {
        item->print(indent + 1);
    }
}
