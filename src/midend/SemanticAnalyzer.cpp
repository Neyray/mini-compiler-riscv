#include "SemanticAnalyzer.h"

#include <typeinfo>

CheckedAST SemanticAnalyzer::check(CompUnitNode* root) {
    if (root == nullptr) {
        throw SemanticError("empty program");
    }

    symbols.enterScope();
    checkCompUnit(root);

    Symbol* mainSym = symbols.lookup("main");
    if (mainSym == nullptr || mainSym->kind != SymbolKind::Function) {
        throw SemanticError("missing main function");
    }
    if (mainSym->returnType != ValueType::Int || !mainSym->paramTypes.empty()) {
        throw SemanticError("main must be int main()");
    }

    symbols.exitScope();
    return CheckedAST(root);
}

void SemanticAnalyzer::checkCompUnit(CompUnitNode* node) {
    for (const auto& item : node->items) {
        checkTopLevelItem(item.get());
    }
}

void SemanticAnalyzer::checkTopLevelItem(ASTNode* node) {
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        checkGlobalDecl(decl);
        return;
    }

    if (auto* func = dynamic_cast<FuncDefNode*>(node)) {
        checkFuncDef(func);
        return;
    }

    throw SemanticError("only declaration or function definition is allowed at global scope");
}

void SemanticAnalyzer::checkGlobalDecl(DeclStmtNode* node) {
    if (node == nullptr) {
        return;
    }

    if (symbols.currentScopeContains(node->name)) {
        throw SemanticError("redefinition of global identifier: " + node->name);
    }
    if (node->initExpr == nullptr) {
        throw SemanticError("declaration must have initializer: " + node->name);
    }

    ValueType initType = checkExpr(node->initExpr.get());
    if (initType != ValueType::Int) {
        throw SemanticError("initializer of " + node->name + " must be int");
    }

    Symbol sym;
    sym.name = node->name;
    sym.kind = node->isConst ? SymbolKind::Constant : SymbolKind::Variable;
    sym.type = ValueType::Int;
    sym.isConst = node->isConst;
    sym.isGlobal = true;

    // 全局初始化式在编译期求值：const 是语义要求，普通全局变量也需要静态初值。
    sym.hasConstValue = true;
    sym.constValue = evalConstExpr(node->initExpr.get());

    if (!symbols.declare(sym)) {
        throw SemanticError("redefinition of global identifier: " + node->name);
    }
}

void SemanticAnalyzer::checkFuncDef(FuncDefNode* node) {
    if (node == nullptr) {
        return;
    }

    if (!symbols.isGlobalScope()) {
        throw SemanticError("function can only be defined at global scope: " + node->name);
    }
    if (symbols.currentScopeContains(node->name)) {
        throw SemanticError("redefinition of function: " + node->name);
    }

    ValueType retType = stringToType(node->returnType);

    Symbol func;
    func.name = node->name;
    func.kind = SymbolKind::Function;
    func.type = retType;
    func.returnType = retType;
    func.isGlobal = true;
    for (size_t i = 0; i < node->params.size(); ++i) {
        func.paramTypes.push_back(ValueType::Int);
    }

    // 先登记函数名，再检查函数体，这样支持函数内部递归调用自己。
    if (!symbols.declare(func)) {
        throw SemanticError("redefinition of function: " + node->name);
    }

    ValueType oldReturnType = currentReturnType;
    currentReturnType = retType;

    // 形参和函数体最外层共用一个作用域。
    // 这样可防止在函数体最外层重复定义形参名；嵌套 Block 仍可遮蔽外层变量。
    symbols.enterScope();

    for (const auto& paramName : node->params) {
        if (symbols.currentScopeContains(paramName)) {
            throw SemanticError("redefinition of parameter: " + paramName);
        }

        Symbol param;
        param.name = paramName;
        param.kind = SymbolKind::Parameter;
        param.type = ValueType::Int;
        param.isConst = false;
        param.isGlobal = false;

        if (!symbols.declare(param)) {
            throw SemanticError("redefinition of parameter: " + paramName);
        }
    }

    checkBlock(node->body.get(), false);

    if (retType == ValueType::Int && !blockAlwaysReturns(node->body.get())) {
        throw SemanticError("int function must return a value on every path: " + node->name);
    }

    symbols.exitScope();
    currentReturnType = oldReturnType;
}

void SemanticAnalyzer::checkBlock(BlockNode* node, bool createScope) {
    if (node == nullptr) {
        return;
    }

    if (createScope) {
        symbols.enterScope();
    }

    for (const auto& stmt : node->stmts) {
        checkStmt(stmt.get());
    }

    if (createScope) {
        symbols.exitScope();
    }
}

void SemanticAnalyzer::checkStmt(StmtNode* node) {
    if (node == nullptr) {
        return;
    }

    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        checkBlock(block, true);
        return;
    }

    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        if (symbols.currentScopeContains(decl->name)) {
            throw SemanticError("redefinition of local identifier: " + decl->name);
        }
        if (decl->initExpr == nullptr) {
            throw SemanticError("declaration must have initializer: " + decl->name);
        }

        ValueType initType = checkExpr(decl->initExpr.get());
        if (initType != ValueType::Int) {
            throw SemanticError("initializer of " + decl->name + " must be int");
        }

        Symbol sym;
        sym.name = decl->name;
        sym.kind = decl->isConst ? SymbolKind::Constant : SymbolKind::Variable;
        sym.type = ValueType::Int;
        sym.isConst = decl->isConst;
        sym.isGlobal = false;

        if (decl->isConst) {
            sym.hasConstValue = true;
            sym.constValue = evalConstExpr(decl->initExpr.get());
        }

        if (!symbols.declare(sym)) {
            throw SemanticError("redefinition of local identifier: " + decl->name);
        }
        return;
    }

    if (auto* assign = dynamic_cast<AssignNode*>(node)) {
        Symbol* sym = symbols.lookup(assign->name);
        if (sym == nullptr) {
            throw SemanticError("undefined variable: " + assign->name);
        }
        if (sym->kind == SymbolKind::Function) {
            throw SemanticError("function cannot be assigned: " + assign->name);
        }
        if (sym->isConst) {
            throw SemanticError("cannot assign to const: " + assign->name);
        }
        if (assign->rhs == nullptr || checkExpr(assign->rhs.get()) != ValueType::Int) {
            throw SemanticError("right side of assignment must be int: " + assign->name);
        }
        return;
    }

    if (auto* ifNode = dynamic_cast<IfNode*>(node)) {
        if (ifNode->condition == nullptr || checkExpr(ifNode->condition.get()) != ValueType::Int) {
            throw SemanticError("if condition must be int");
        }
        checkStmt(ifNode->thenStmt.get());
        if (ifNode->elseStmt != nullptr) {
            checkStmt(ifNode->elseStmt.get());
        }
        return;
    }

    if (auto* whileNode = dynamic_cast<WhileNode*>(node)) {
        if (whileNode->condition == nullptr || checkExpr(whileNode->condition.get()) != ValueType::Int) {
            throw SemanticError("while condition must be int");
        }
        loopDepth++;
        checkStmt(whileNode->body.get());
        loopDepth--;
        return;
    }

    if (dynamic_cast<BreakNode*>(node)) {
        if (loopDepth == 0) {
            throw SemanticError("break outside loop");
        }
        return;
    }

    if (dynamic_cast<ContinueNode*>(node)) {
        if (loopDepth == 0) {
            throw SemanticError("continue outside loop");
        }
        return;
    }

    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        if (currentReturnType == ValueType::Void) {
            if (ret->retExpr != nullptr) {
                throw SemanticError("void function should not return a value");
            }
        } else {
            if (ret->retExpr == nullptr) {
                throw SemanticError("int function must return a value");
            }
            if (checkExpr(ret->retExpr.get()) != ValueType::Int) {
                throw SemanticError("return expression must be int");
            }
        }
        return;
    }

    if (auto* exprStmt = dynamic_cast<ExprStmtNode*>(node)) {
        if (exprStmt->expr != nullptr) {
            // 表达式语句允许 void 函数调用，例如 foo();
            checkExpr(exprStmt->expr.get());
        }
        return;
    }

    throw SemanticError("unknown statement node");
}

ValueType SemanticAnalyzer::checkExpr(ExprNode* node) {
    if (node == nullptr) {
        throw SemanticError("empty expression");
    }

    if (dynamic_cast<LiteralNode*>(node)) {
        return ValueType::Int;
    }

    if (auto* var = dynamic_cast<VarNode*>(node)) {
        Symbol* sym = symbols.lookup(var->name);
        if (sym == nullptr) {
            throw SemanticError("undefined identifier: " + var->name);
        }
        if (sym->kind == SymbolKind::Function) {
            throw SemanticError("function cannot be used as variable: " + var->name);
        }
        return ValueType::Int;
    }

    if (auto* unary = dynamic_cast<UnaryNode*>(node)) {
        ValueType t = checkExpr(unary->operand.get());
        if (t != ValueType::Int) {
            throw SemanticError("unary operator requires int operand: " + unary->op);
        }
        return ValueType::Int;
    }

    if (auto* binary = dynamic_cast<BinaryNode*>(node)) {
        ValueType lt = checkExpr(binary->left.get());
        ValueType rt = checkExpr(binary->right.get());
        if (lt != ValueType::Int || rt != ValueType::Int) {
            throw SemanticError("binary operator requires int operands: " + binary->op);
        }
        return ValueType::Int;
    }

    if (auto* call = dynamic_cast<CallNode*>(node)) {
        Symbol* sym = symbols.lookup(call->funcName);
        if (sym == nullptr || sym->kind != SymbolKind::Function) {
            throw SemanticError("undefined function: " + call->funcName);
        }
        if (sym->paramTypes.size() != call->args.size()) {
            throw SemanticError("function argument count mismatch: " + call->funcName);
        }
        for (const auto& arg : call->args) {
            if (checkExpr(arg.get()) != ValueType::Int) {
                throw SemanticError("function argument must be int: " + call->funcName);
            }
        }
        return sym->returnType;
    }

    throw SemanticError("unknown expression node");
}

bool SemanticAnalyzer::stmtAlwaysReturns(StmtNode* node) {
    if (node == nullptr) {
        return false;
    }

    if (dynamic_cast<ReturnNode*>(node)) {
        return true;
    }

    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        return blockAlwaysReturns(block);
    }

    if (auto* ifNode = dynamic_cast<IfNode*>(node)) {
        if (ifNode->elseStmt == nullptr) {
            return false;
        }
        return stmtAlwaysReturns(ifNode->thenStmt.get()) &&
               stmtAlwaysReturns(ifNode->elseStmt.get());
    }

    if (auto* whileNode = dynamic_cast<WhileNode*>(node)) {
        // while(常量真) 无法从条件处正常退出：若函数以这样的循环收尾，
        // 视为已覆盖所有路径，避免误判「缺少 return」。非常量条件保守处理。
        try {
            if (evalConstExpr(whileNode->condition.get()) != 0) {
                return true;
            }
        } catch (const SemanticError&) {
            // 条件不是编译期常量，按不必然返回处理。
        }
        return false;
    }

    return false;
}

bool SemanticAnalyzer::blockAlwaysReturns(BlockNode* node) {
    if (node == nullptr) {
        return false;
    }

    for (const auto& stmt : node->stmts) {
        if (stmtAlwaysReturns(stmt.get())) {
            return true;
        }
    }

    return false;
}

int SemanticAnalyzer::evalConstExpr(ExprNode* node) {
    if (node == nullptr) {
        throw SemanticError("empty const expression");
    }

    if (auto* lit = dynamic_cast<LiteralNode*>(node)) {
        return lit->value;
    }

    if (auto* var = dynamic_cast<VarNode*>(node)) {
        Symbol* sym = symbols.lookup(var->name);
        if (sym == nullptr) {
            throw SemanticError("undefined identifier in const expression: " + var->name);
        }
        if (!sym->isConst || !sym->hasConstValue) {
            throw SemanticError("const initializer can only use literal or declared const: " + var->name);
        }
        return sym->constValue;
    }

    if (auto* unary = dynamic_cast<UnaryNode*>(node)) {
        int v = evalConstExpr(unary->operand.get());
        if (unary->op == "+") return v;
        if (unary->op == "-") return -v;
        if (unary->op == "!") return !v;
        throw SemanticError("unknown unary operator in const expression: " + unary->op);
    }

    if (auto* binary = dynamic_cast<BinaryNode*>(node)) {
        int l = evalConstExpr(binary->left.get());
        int r = evalConstExpr(binary->right.get());

        if (binary->op == "+") return l + r;
        if (binary->op == "-") return l - r;
        if (binary->op == "*") return l * r;
        if (binary->op == "/") {
            if (r == 0) throw SemanticError("division by zero in const expression");
            return l / r;
        }
        if (binary->op == "%") {
            if (r == 0) throw SemanticError("modulo by zero in const expression");
            return l % r;
        }
        if (binary->op == "<") return l < r;
        if (binary->op == ">") return l > r;
        if (binary->op == "<=") return l <= r;
        if (binary->op == ">=") return l >= r;
        if (binary->op == "==") return l == r;
        if (binary->op == "!=") return l != r;
        if (binary->op == "&&") return (l != 0) && (r != 0);
        if (binary->op == "||") return (l != 0) || (r != 0);

        throw SemanticError("unknown binary operator in const expression: " + binary->op);
    }

    if (auto* call = dynamic_cast<CallNode*>(node)) {
        throw SemanticError("function call is not allowed in const expression: " + call->funcName);
    }

    throw SemanticError("unknown const expression node");
}

ValueType SemanticAnalyzer::stringToType(const std::string& s) const {
    if (s == "int") {
        return ValueType::Int;
    }
    if (s == "void") {
        return ValueType::Void;
    }
    throw SemanticError("unknown type: " + s);
}

std::string SemanticAnalyzer::typeToString(ValueType t) const {
    return t == ValueType::Int ? "int" : "void";
}
