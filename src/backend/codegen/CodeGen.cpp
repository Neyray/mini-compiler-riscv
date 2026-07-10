#include "CodeGen.h"
#include <algorithm>
#include <stdexcept>

CodeGen::CodeGen(bool optimize)
    : opt(optimize), labelCounter(0), totalLocals(0), regLocals(0),
      stackLocalBase(0), tempBaseOffset(0), localCursor(0), funcHasCall(false),
      aRegLeaf(false), tempBias(0) {}

// ---------- 作用域 ----------

void CodeGen::pushScope() {
    scopes.push_back({});
}

void CodeGen::popScope() {
    scopes.pop_back();
}

const CodeGen::VarInfo* CodeGen::lookupVar(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

CodeGen::VarInfo* CodeGen::lookupVarMutable(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

void CodeGen::clearMutableKnownValues() {
    for (auto& scope : scopes) {
        for (auto& [_, info] : scope) {
            if (info.kind != VarInfo::Const) info.hasKnownValue = false;
        }
    }
}

void CodeGen::clearPureExprCache() {
    cachedPureExpr = nullptr;
}

void CodeGen::cachePureExpr(ExprNode* expr, const VarInfo& value) {
    if (opt && expr != nullptr && !containsCall(expr) &&
        (value.kind == VarInfo::Reg || value.kind == VarInfo::Local)) {
        cachedPureExpr = expr;
        cachedPureValue = value;
    } else {
        clearPureExprCache();
    }
}

bool CodeGen::tryLoadCachedPureExpr(ExprNode* node) {
    if (!opt || cachedPureExpr == nullptr || containsCall(node) ||
        !exprStructurallyEqual(cachedPureExpr, node)) {
        return false;
    }

    if (cachedPureValue.kind == VarInfo::Reg) {
        emit("mv a0, " + cachedPureValue.reg);
    } else if (cachedPureValue.kind == VarInfo::Local) {
        memInsn("lw", "a0", cachedPureValue.offset);
    } else {
        return false;
    }
    return true;
}

// 为一个局部声明/形参分配存储。
// 叶子无栈帧模式：全部落入调用者保存寄存器 a1..a7（第 idx 个 → a(idx+1)）。
// 普通模式：前 11 个进 s1..s11，其余落栈。
CodeGen::VarInfo CodeGen::assignLocalLocation() {
    int idx = localCursor++;
    VarInfo info;
    if (aRegLeaf) {
        info.kind = VarInfo::Reg;
        info.reg = "a" + std::to_string(1 + idx);
        return info;
    }
    if (idx < 11) {
        info.kind = VarInfo::Reg;
        info.reg = "s" + std::to_string(1 + idx);
    } else {
        info.kind = VarInfo::Local;
        info.offset = stackLocalBase - 4 * (idx - 11);
    }
    return info;
}

bool CodeGen::tryGetReg(ExprNode* node, std::string& reg) const {
    auto* var = dynamic_cast<VarNode*>(node);
    if (var == nullptr) return false;

    const VarInfo* info = lookupVar(var->name);
    if (info == nullptr || info->kind != VarInfo::Reg) return false;

    reg = info->reg;
    return true;
}

// ---------- 发射辅助 ----------

void CodeGen::emit(const std::string& line) {
    cur.push_back("\t" + line);
}

void CodeGen::emitLabel(const std::string& label) {
    cur.push_back(label + ":");
}

std::string CodeGen::newLabel() {
    return ".L" + std::to_string(labelCounter++);
}

void CodeGen::loadImm(const std::string& reg, int value) {
    emit("li " + reg + ", " + std::to_string(value));
}

// 以 s0 为基址的 load/store，处理超出 12 位立即数范围的偏移（用 t6 暂存地址）。
void CodeGen::memInsn(const std::string& op, const std::string& reg, int offset) {
    if (offset >= -2048 && offset <= 2047) {
        emit(op + " " + reg + ", " + std::to_string(offset) + "(s0)");
    } else {
        emit("li t6, " + std::to_string(offset));
        emit("add t6, s0, t6");
        emit(op + " " + reg + ", 0(t6)");
    }
}

int CodeGen::tempOffset(int slot) const {
    return tempBaseOffset - 4 * (slot + tempBias);
}

void CodeGen::storeTemp(int slot) {
    memInsn("sw", "a0", tempOffset(slot));
}

void CodeGen::loadTemp(int slot, const std::string& reg) {
    memInsn("lw", reg, tempOffset(slot));
}

// ---------- 预分析 ----------

int CodeGen::countLocalDecls(StmtNode* node) const {
    if (node == nullptr) return 0;
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        return (decl->isConst ? 0 : 1) + inlineLocalsInExpr(decl->initExpr.get());
    }
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        int n = 0;
        for (const auto& s : block->stmts) n += countLocalDecls(s.get());
        return n;
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        return inlineLocalsInExpr(asn->rhs.get());
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        return inlineLocalsInExpr(ifn->condition.get()) +
               countLocalDecls(ifn->thenStmt.get()) + countLocalDecls(ifn->elseStmt.get());
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        return inlineLocalsInExpr(wh->condition.get()) + countLocalDecls(wh->body.get());
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        return inlineLocalsInExpr(ret->retExpr.get());
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        return inlineLocalsInExpr(es->expr.get());
    }
    return 0;
}

// 表达式内所有可内联调用点将额外占用的局部存储（形参 + 被内联函数体的局部）。
// 被内联函数是叶子（不含调用），故递归不会无限展开。
int CodeGen::inlineLocalsInExpr(ExprNode* node) const {
    if (node == nullptr) return 0;
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        return inlineLocalsInExpr(un->operand.get());
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        return inlineLocalsInExpr(bin->left.get()) + inlineLocalsInExpr(bin->right.get());
    }
    if (auto* call = dynamic_cast<CallNode*>(node)) {
        int n = 0;
        for (const auto& a : call->args) n += inlineLocalsInExpr(a.get());
        if (inlinableFuncs.count(call->funcName)) {
            FuncDefNode* f = funcDefs.at(call->funcName);
            n += static_cast<int>(f->params.size()) + countLocalDecls(f->body.get());
        }
        return n;
    }
    return 0;
}

int CodeGen::exprTempNeed(ExprNode* node) const {
    if (node == nullptr) return 0;
    if (dynamic_cast<LiteralNode*>(node) || dynamic_cast<VarNode*>(node)) {
        return 0;
    }
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        return exprTempNeed(un->operand.get());
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        int l = exprTempNeed(bin->left.get());
        int r = exprTempNeed(bin->right.get());
        if (bin->op == "&&" || bin->op == "||") {
            return l > r ? l : r;
        }
        int rr = 1 + r;
        return l > rr ? l : rr;
    }
    if (auto* call = dynamic_cast<CallNode*>(node)) {
        int n = static_cast<int>(call->args.size());
        if (opt && inlinableFuncs.count(call->funcName)) {
            // 内联展开：实参依次在当前深度求值（结果直接进形参存储，不占槽），
            // 之后函数体的临时槽经 tempBias 平移到当前深度之上。
            int best = stmtTempNeed(funcDefs.at(call->funcName)->body.get());
            for (int i = 0; i < n; ++i) {
                int need = exprTempNeed(call->args[i].get());
                if (need > best) best = need;
            }
            return best;
        }
        int best = n;
        for (int i = 0; i < n; ++i) {
            int need = i + exprTempNeed(call->args[i].get());
            if (need > best) best = need;
        }
        return best;
    }
    return 0;
}

// ---------- AST 规模统计（内联判定） ----------

int CodeGen::nodeCountExpr(ExprNode* node) const {
    if (node == nullptr) return 0;
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        return 1 + nodeCountExpr(un->operand.get());
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        return 1 + nodeCountExpr(bin->left.get()) + nodeCountExpr(bin->right.get());
    }
    if (auto* call = dynamic_cast<CallNode*>(node)) {
        int n = 1;
        for (const auto& a : call->args) n += nodeCountExpr(a.get());
        return n;
    }
    return 1;
}

int CodeGen::nodeCountStmt(StmtNode* node) const {
    if (node == nullptr) return 0;
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        int n = 1;
        for (const auto& s : block->stmts) n += nodeCountStmt(s.get());
        return n;
    }
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        return 1 + nodeCountExpr(decl->initExpr.get());
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        return 1 + nodeCountExpr(asn->rhs.get());
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        return 1 + nodeCountExpr(ifn->condition.get()) +
               nodeCountStmt(ifn->thenStmt.get()) + nodeCountStmt(ifn->elseStmt.get());
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        return 1 + nodeCountExpr(wh->condition.get()) + nodeCountStmt(wh->body.get());
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        return 1 + nodeCountExpr(ret->retExpr.get());
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        return 1 + nodeCountExpr(es->expr.get());
    }
    return 1;
}

// ---------- 条件常量提升的收集 ----------

// 从一个条件表达式中挑出参与关系比较的非零字面量（0 直接用 zero 寄存器，无需提升）。
void CodeGen::collectCondConstLits(ExprNode* cond, std::vector<int>& out) const {
    if (cond == nullptr) return;
    if (auto* un = dynamic_cast<UnaryNode*>(cond)) {
        if (un->op == "!") collectCondConstLits(un->operand.get(), out);
        return;
    }
    auto* bin = dynamic_cast<BinaryNode*>(cond);
    if (bin == nullptr) return;
    if (bin->op == "&&" || bin->op == "||") {
        collectCondConstLits(bin->left.get(), out);
        collectCondConstLits(bin->right.get(), out);
        return;
    }
    const std::string& op = bin->op;
    if (op != "<" && op != ">" && op != "<=" && op != ">=" && op != "==" && op != "!=") return;
    for (ExprNode* side : {bin->left.get(), bin->right.get()}) {
        auto* lit = dynamic_cast<LiteralNode*>(side);
        if (lit != nullptr && lit->value != 0 &&
            std::find(out.begin(), out.end(), lit->value) == out.end()) {
            out.push_back(lit->value);
        }
    }
}

// 收集执行频率高的条件比较常量：while 条件总是收集；if 条件仅在循环内收集。
// 深入可内联调用点的函数体（内联后这些条件同样出现在本函数中）。
void CodeGen::collectHoistConstsExpr(ExprNode* node, std::vector<int>& out, bool inLoop) const {
    if (node == nullptr) return;
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        collectHoistConstsExpr(un->operand.get(), out, inLoop);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        // 循环内算术运算的常量操作数：乘/除/模任何非零常量都需要 li 装载；
        // 加/减仅当超出 12 位立即数范围（否则 addi 单指令即可）。提升后每轮省一条 li。
        if (inLoop) {
            auto consider = [&](ExprNode* side) {
                auto* lit = dynamic_cast<LiteralNode*>(side);
                if (lit == nullptr || lit->value == 0) return;
                bool needReg =
                    (bin->op == "*" || bin->op == "/" || bin->op == "%") ||
                    ((bin->op == "+" || bin->op == "-") &&
                     (lit->value < -2048 || lit->value > 2047));
                if (needReg &&
                    std::find(out.begin(), out.end(), lit->value) == out.end()) {
                    out.push_back(lit->value);
                }
            };
            consider(bin->left.get());
            consider(bin->right.get());
        }
        collectHoistConstsExpr(bin->left.get(), out, inLoop);
        collectHoistConstsExpr(bin->right.get(), out, inLoop);
        return;
    }
    if (auto* call = dynamic_cast<CallNode*>(node)) {
        for (const auto& a : call->args) collectHoistConstsExpr(a.get(), out, inLoop);
        if (inlinableFuncs.count(call->funcName)) {
            collectHoistConsts(funcDefs.at(call->funcName)->body.get(), out, inLoop);
        }
    }
}

void CodeGen::collectHoistConsts(StmtNode* node, std::vector<int>& out, bool inLoop) const {
    if (node == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        for (const auto& s : block->stmts) collectHoistConsts(s.get(), out, inLoop);
        return;
    }
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        collectHoistConstsExpr(decl->initExpr.get(), out, inLoop);
        return;
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        collectHoistConstsExpr(asn->rhs.get(), out, inLoop);
        return;
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        if (inLoop) collectCondConstLits(ifn->condition.get(), out);
        collectHoistConstsExpr(ifn->condition.get(), out, inLoop);
        collectHoistConsts(ifn->thenStmt.get(), out, inLoop);
        collectHoistConsts(ifn->elseStmt.get(), out, inLoop);
        return;
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        collectCondConstLits(wh->condition.get(), out);
        collectHoistConstsExpr(wh->condition.get(), out, true);
        collectHoistConsts(wh->body.get(), out, true);
        return;
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        collectHoistConstsExpr(ret->retExpr.get(), out, inLoop);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        collectHoistConstsExpr(es->expr.get(), out, inLoop);
        return;
    }
}

int CodeGen::stmtTempNeed(StmtNode* node) const {
    if (node == nullptr) return 0;
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        int m = 0;
        for (const auto& s : block->stmts) {
            int t = stmtTempNeed(s.get());
            if (t > m) m = t;
        }
        return m;
    }
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        return exprTempNeed(decl->initExpr.get());
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        return exprTempNeed(asn->rhs.get());
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        int a = exprTempNeed(ifn->condition.get());
        int b = stmtTempNeed(ifn->thenStmt.get());
        int c = stmtTempNeed(ifn->elseStmt.get());
        int m = a > b ? a : b;
        return m > c ? m : c;
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        int a = exprTempNeed(wh->condition.get());
        int b = stmtTempNeed(wh->body.get());
        return a > b ? a : b;
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        return exprTempNeed(ret->retExpr.get());
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        return exprTempNeed(es->expr.get());
    }
    return 0;
}

bool CodeGen::containsCall(ExprNode* node) const {
    if (node == nullptr) return false;
    if (dynamic_cast<CallNode*>(node)) return true;
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        return containsCall(un->operand.get());
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        return containsCall(bin->left.get()) || containsCall(bin->right.get());
    }
    return false;
}

bool CodeGen::stmtContainsCall(StmtNode* node) const {
    if (node == nullptr) return false;
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        for (const auto& s : block->stmts) if (stmtContainsCall(s.get())) return true;
        return false;
    }
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        return containsCall(decl->initExpr.get());
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        return containsCall(asn->rhs.get());
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        return containsCall(ifn->condition.get()) ||
               stmtContainsCall(ifn->thenStmt.get()) ||
               stmtContainsCall(ifn->elseStmt.get());
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        return containsCall(wh->condition.get()) || stmtContainsCall(wh->body.get());
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        return containsCall(ret->retExpr.get());
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        return containsCall(es->expr.get());
    }
    return false;
}

// 表达式求值可能达到的最大嵌套深度（上界）。genBinaryOperands 只在 depth>=6 时
// 才把左操作数溢出到栈临时槽；若整段代码的最大深度 <6，则叶子函数无需任何栈临时槽。
// 这里按“所有二元运算都走通用路径”估计（右子树深度 +1），是安全的上界。
int CodeGen::exprMaxDepth(ExprNode* node, int depth) const {
    if (node == nullptr) return depth;
    if (dynamic_cast<LiteralNode*>(node) || dynamic_cast<VarNode*>(node)) return depth;
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        return exprMaxDepth(un->operand.get(), depth);
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        int l = exprMaxDepth(bin->left.get(), depth);
        int rdepth = (bin->op == "&&" || bin->op == "||") ? depth : depth + 1;
        int r = exprMaxDepth(bin->right.get(), rdepth);
        return l > r ? l : r;
    }
    // 含调用的表达式不会用于叶子无栈帧判定，返回大值以排除。
    if (dynamic_cast<CallNode*>(node)) return 1000;
    return depth;
}

int CodeGen::stmtMaxDepth(StmtNode* node) const {
    if (node == nullptr) return 0;
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        int m = 0;
        for (const auto& s : block->stmts) {
            int t = stmtMaxDepth(s.get());
            if (t > m) m = t;
        }
        return m;
    }
    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        return exprMaxDepth(decl->initExpr.get(), 0);
    }
    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        return exprMaxDepth(asn->rhs.get(), 0);
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        int a = exprMaxDepth(ifn->condition.get(), 0);
        int b = stmtMaxDepth(ifn->thenStmt.get());
        int c = stmtMaxDepth(ifn->elseStmt.get());
        int m = a > b ? a : b;
        return m > c ? m : c;
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        int a = exprMaxDepth(wh->condition.get(), 0);
        int b = stmtMaxDepth(wh->body.get());
        return a > b ? a : b;
    }
    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        return exprMaxDepth(ret->retExpr.get(), 0);
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        return exprMaxDepth(es->expr.get(), 0);
    }
    return 0;
}

// ---------- 编译期常量求值 ----------

bool CodeGen::tryEvalConst(ExprNode* node, int& out) const {
    if (node == nullptr) return false;
    if (auto* lit = dynamic_cast<LiteralNode*>(node)) {
        out = lit->value;
        return true;
    }
    if (auto* var = dynamic_cast<VarNode*>(node)) {
        const VarInfo* v = lookupVar(var->name);
        if (v != nullptr && v->kind == VarInfo::Const) {
            out = v->constVal;
            return true;
        }
        if (opt && v != nullptr && v->hasKnownValue) {
            out = v->knownValue;
            return true;
        }
        return false;
    }
    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        int v;
        if (!tryEvalConst(un->operand.get(), v)) return false;
        if (un->op == "+") { out = v; return true; }
        if (un->op == "-") { out = -v; return true; }
        if (un->op == "!") { out = (v == 0) ? 1 : 0; return true; }
        return false;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        int l, r;
        if (!tryEvalConst(bin->left.get(), l)) return false;
        if (bin->op == "&&") {
            if (l == 0) { out = 0; return true; }
            if (!tryEvalConst(bin->right.get(), r)) return false;
            out = (r != 0) ? 1 : 0;
            return true;
        }
        if (bin->op == "||") {
            if (l != 0) { out = 1; return true; }
            if (!tryEvalConst(bin->right.get(), r)) return false;
            out = (r != 0) ? 1 : 0;
            return true;
        }
        if (!tryEvalConst(bin->right.get(), r)) return false;
        if (bin->op == "+") { out = l + r; return true; }
        if (bin->op == "-") { out = l - r; return true; }
        if (bin->op == "*") { out = l * r; return true; }
        if (bin->op == "/") { if (r == 0) return false; out = l / r; return true; }
        if (bin->op == "%") { if (r == 0) return false; out = l % r; return true; }
        if (bin->op == "<")  { out = l <  r; return true; }
        if (bin->op == ">")  { out = l >  r; return true; }
        if (bin->op == "<=") { out = l <= r; return true; }
        if (bin->op == ">=") { out = l >= r; return true; }
        if (bin->op == "==") { out = l == r; return true; }
        if (bin->op == "!=") { out = l != r; return true; }
        return false;
    }
    return false;
}

// ---------- 顶层 ----------

void CodeGen::generate(CompUnitNode* root, std::ostream& out) {
    scopes.clear();
    pushScope();

    for (const auto& item : root->items) {
        if (auto* decl = dynamic_cast<DeclStmtNode*>(item.get())) {
            int v = 0;
            bool isConstVal = tryEvalConst(decl->initExpr.get(), v);
            if (decl->isConst) {
                VarInfo info;
                info.kind = VarInfo::Const;
                info.constVal = v;
                scopes[0][decl->name] = info;
            } else {
                VarInfo info;
                info.kind = VarInfo::Global;
                info.label = "g_" + decl->name;
                scopes[0][decl->name] = info;
                dataLines.push_back("g_" + decl->name + ":");
                dataLines.push_back("\t.word " + std::to_string(isConstVal ? v : 0));
            }
        }
    }

    funcDefs.clear();
    inlinableFuncs.clear();
    for (const auto& item : root->items) {
        if (auto* func = dynamic_cast<FuncDefNode*>(item.get())) {
            funcDefs[func->name] = func;
        }
    }
    if (opt) {
        // 可内联判定：叶子函数（不含任何调用，天然排除递归）、局部与形参总量
        // 能进寄存器、函数体规模小。此时 inlinableFuncs 为空，countLocalDecls
        // 不会额外计数，判定结果与生成阶段一致。
        for (const auto& [name, f] : funcDefs) {
            if (stmtContainsCall(f->body.get())) continue;
            int locals = static_cast<int>(f->params.size()) + countLocalDecls(f->body.get());
            if (locals > 8) continue;
            if (nodeCountStmt(f->body.get()) > 48) continue;
            inlinableFuncs.insert(name);
        }
    }

    for (const auto& item : root->items) {
        if (auto* func = dynamic_cast<FuncDefNode*>(item.get())) {
            genFunc(func);
        }
    }

    out << "\t.text\n";
    for (const auto& l : textLines) out << l << "\n";
    if (!dataLines.empty()) {
        out << "\t.data\n";
        for (const auto& l : dataLines) out << l << "\n";
    }
}

void CodeGen::genFunc(FuncDefNode* func) {
    cur.clear();
    loopLabels.clear();
    condConstRegs.clear();
    tempBias = 0;
    currentFunctionName = func->name;
    currentFunctionParams = func->params;
    tailEntryLabel.clear();
    clearPureExprCache();

    int nparams = static_cast<int>(func->params.size());
    totalLocals = nparams + countLocalDecls(func->body.get());
    localCursor = 0;
    funcHasCall = stmtContainsCall(func->body.get());

    // 叶子无栈帧模式：无函数调用、局部数 <=7（可全部放入 a1..a7），
    // 且表达式嵌套深度 <6（不会产生栈上临时槽）。此时函数只使用调用者保存寄存器，
    // 天然保持全部被调用者保存寄存器不变，可完全省去栈帧与序言/尾声。
    aRegLeaf = opt && !funcHasCall && totalLocals <= 7 &&
               stmtMaxDepth(func->body.get()) < 6;

    epilogueLabel = newLabel();
    emit(".globl " + func->name);
    emitLabel(func->name);

    if (aRegLeaf) {
        pushScope();
        // 形参各自的寄存器家：第 i 个形参 a_i -> a_(i+1)。
        for (int i = 0; i < nparams; ++i) {
            scopes.back()[func->params[i]] = assignLocalLocation();
        }
        // 逆序搬移，避免把尚未读取的后续入参覆盖掉。
        for (int i = nparams - 1; i >= 0; --i) {
            emit("mv a" + std::to_string(i + 1) + ", a" + std::to_string(i));
        }
        // 条件常量提升：把循环中频繁比较的字面量装入剩余的 a 寄存器
        // （叶子函数无调用，a 寄存器在函数内稳定）。
        if (opt) {
            std::vector<int> hoistedLeaf;
            collectHoistConsts(func->body.get(), hoistedLeaf, false);
            int availLeaf = 7 - totalLocals;
            if (availLeaf < 0) availLeaf = 0;
            if (static_cast<int>(hoistedLeaf.size()) > availLeaf) hoistedLeaf.resize(availLeaf);
            for (int i = 0; i < static_cast<int>(hoistedLeaf.size()); ++i) {
                std::string reg = "a" + std::to_string(1 + totalLocals + i);
                condConstRegs[hoistedLeaf[i]] = reg;
                loadImm(reg, hoistedLeaf[i]);
            }
        }
        tailEntryLabel = newLabel();
        emitLabel(tailEntryLabel);
        genBlock(func->body.get(), false);
        emitLabel(epilogueLabel);
        emit("ret");
        popScope();
        peephole();
        for (const auto& l : cur) textLines.push_back(l);
        return;
    }

    regLocals = totalLocals < 11 ? totalLocals : 11;
    int stackLocals = totalLocals - regLocals;
    int temps = stmtTempNeed(func->body.get());

    // 条件常量提升：把循环中频繁比较的字面量装入空闲的 s 寄存器（s 寄存器跨调用
    // 保持，故对含调用的循环体同样有效），循环内每轮省下一条 li。
    std::vector<int> hoisted;
    if (opt) {
        collectHoistConsts(func->body.get(), hoisted, false);
        int avail = 11 - regLocals;
        if (avail < 0) avail = 0;
        if (static_cast<int>(hoisted.size()) > avail) hoisted.resize(avail);
        for (int i = 0; i < static_cast<int>(hoisted.size()); ++i) {
            condConstRegs[hoisted[i]] = "s" + std::to_string(1 + regLocals + i);
        }
    }
    int savedRegs = regLocals + static_cast<int>(hoisted.size());

    stackLocalBase = -12 - 4 * savedRegs;
    tempBaseOffset = stackLocalBase - 4 * stackLocals;

    int frameWords = 2 + savedRegs + stackLocals + temps;
    int frame = (4 * frameWords + 15) & ~15;

    // 序言：分配帧，保存 ra（非叶子）、旧 s0、用到的 s1..s(regLocals)，建立 s0。
    bool smallFrame = frame <= 2047;
    if (smallFrame) {
        emit("addi sp, sp, -" + std::to_string(frame));
        emit("addi t0, sp, " + std::to_string(frame));
    } else {
        emit("li t0, " + std::to_string(frame));
        emit("sub sp, sp, t0");
        emit("add t0, sp, t0");
    }
    if (funcHasCall) emit("sw ra, -4(t0)");
    emit("sw s0, -8(t0)");
    for (int i = 0; i < savedRegs; ++i) {
        emit("sw s" + std::to_string(1 + i) + ", " + std::to_string(-12 - 4 * i) + "(t0)");
    }
    emit("mv s0, t0");

    pushScope();

    // 形参就位。
    for (int i = 0; i < nparams; ++i) {
        VarInfo info = assignLocalLocation();
        scopes.back()[func->params[i]] = info;

        if (i < 8) {
            std::string ai = "a" + std::to_string(i);
            if (info.kind == VarInfo::Reg) emit("mv " + info.reg + ", " + ai);
            else memInsn("sw", ai, info.offset);
        } else {
            int inOff = 4 * (i - 8);
            std::string dst = (info.kind == VarInfo::Reg) ? info.reg : "t6";
            if (inOff >= -2048 && inOff <= 2047) {
                emit("lw " + dst + ", " + std::to_string(inOff) + "(s0)");
            } else {
                emit("li t6, " + std::to_string(inOff));
                emit("add t6, s0, t6");
                emit("lw " + dst + ", 0(t6)");
            }
            if (info.kind != VarInfo::Reg) memInsn("sw", "t6", info.offset);
        }
    }

    // 提升的条件常量就位（每个函数只装载一次）。
    for (int i = 0; i < static_cast<int>(hoisted.size()); ++i) {
        loadImm(condConstRegs[hoisted[i]], hoisted[i]);
    }

    tailEntryLabel = newLabel();
    emitLabel(tailEntryLabel);
    genBlock(func->body.get(), false);

    emitLabel(epilogueLabel);
    if (funcHasCall) emit("lw ra, -4(s0)");
    for (int i = 0; i < savedRegs; ++i) {
        emit("lw s" + std::to_string(1 + i) + ", " + std::to_string(-12 - 4 * i) + "(s0)");
    }
    if (smallFrame) {
        emit("mv sp, s0");
        emit("lw s0, -8(s0)");
    } else {
        emit("lw t6, -8(s0)");
        emit("mv sp, s0");
        emit("mv s0, t6");
    }
    emit("ret");

    popScope();

    peephole();

    for (const auto& l : cur) textLines.push_back(l);
}

// ---------- 语句 ----------

void CodeGen::genBlock(BlockNode* node, bool createScope) {
    int savedCursor = localCursor;
    if (createScope) {
        pushScope();
        clearPureExprCache();
    }
    for (const auto& s : node->stmts) {
        genStmt(s.get());
        // 这些直接终结语句后的同一块语句不可达，不生成即可避免死代码进入输出。
        if (opt && (dynamic_cast<ReturnNode*>(s.get()) != nullptr ||
                    dynamic_cast<BreakNode*>(s.get()) != nullptr ||
                    dynamic_cast<ContinueNode*>(s.get()) != nullptr)) {
            break;
        }
    }
    if (createScope) {
        popScope();
        // 块作用域已经退出：后续互斥/顺序块可安全复用其局部寄存器或栈槽。
        localCursor = savedCursor;
        clearPureExprCache();
    }
}

void CodeGen::genStmt(StmtNode* node) {
    if (node == nullptr) return;

    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        genBlock(block, true);
        return;
    }

    if (auto* decl = dynamic_cast<DeclStmtNode*>(node)) {
        if (decl->isConst) {
            int v = 0;
            if (tryEvalConst(decl->initExpr.get(), v)) {
                VarInfo info;
                info.kind = VarInfo::Const;
                info.constVal = v;
                scopes.back()[decl->name] = info;
                clearPureExprCache();
                return;
            }
        }
        VarInfo info = assignLocalLocation();
        int knownValue = 0;
        if (opt && tryEvalConst(decl->initExpr.get(), knownValue)) {
            info.hasKnownValue = true;
            info.knownValue = knownValue;
        }
        bool cacheHit = tryLoadCachedPureExpr(decl->initExpr.get());
        if (!cacheHit) clearPureExprCache();
        if (opt && info.kind == VarInfo::Reg) {
            if (cacheHit) emit("mv " + info.reg + ", a0");
            else genExprInto(decl->initExpr.get(), info.reg, 0);
        } else {
            if (!cacheHit) genExpr(decl->initExpr.get(), 0);
            if (info.kind == VarInfo::Reg) emit("mv " + info.reg + ", a0");
            else memInsn("sw", "a0", info.offset);
        }
        scopes.back()[decl->name] = info;
        cachePureExpr(decl->initExpr.get(), info);
        return;
    }

    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        int knownValue = 0;
        bool rhsKnown = opt && tryEvalConst(asn->rhs.get(), knownValue);
        auto updateKnownValue = [&]() {
            VarInfo* current = lookupVarMutable(asn->name);
            if (current != nullptr && current->kind != VarInfo::Const &&
                current->kind != VarInfo::Global) {
                current->hasKnownValue = rhsKnown;
                current->knownValue = knownValue;
            }
        };
        bool cacheHit = tryLoadCachedPureExpr(asn->rhs.get());
        if (!cacheHit) clearPureExprCache();
        if (!cacheHit && tryEmitOptimizedAssign(asn)) {
            updateKnownValue();
            cachePureExpr(asn->rhs.get(), *lookupVar(asn->name));
            return;
        }

        const VarInfo* found = lookupVar(asn->name);
        if (found == nullptr) throw std::runtime_error("codegen: undefined variable " + asn->name);
        VarInfo v = *found;  // 值拷贝：rhs 内的内联展开会临时切换作用域栈
        if (opt && v.kind == VarInfo::Reg) {
            if (cacheHit) emit("mv " + v.reg + ", a0");
            else genExprInto(asn->rhs.get(), v.reg, 0);
            updateKnownValue();
            cachePureExpr(asn->rhs.get(), v);
            return;
        }
        if (!cacheHit) genExpr(asn->rhs.get(), 0);
        if (v.kind == VarInfo::Global) {
            emit("la t6, " + v.label);
            emit("sw a0, 0(t6)");
        } else if (v.kind == VarInfo::Reg) {
            emit("mv " + v.reg + ", a0");
        } else if (v.kind == VarInfo::Local) {
            memInsn("sw", "a0", v.offset);
        }
        updateKnownValue();
        cachePureExpr(asn->rhs.get(), v);
        return;
    }

    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        int condition;
        if (opt && tryEvalConst(ifn->condition.get(), condition)) {
            if (condition != 0) genStmt(ifn->thenStmt.get());
            else if (ifn->elseStmt != nullptr) genStmt(ifn->elseStmt.get());
            return;
        }
        clearPureExprCache();
        auto savedScopes = scopes;
        std::string lend = newLabel();
        if (ifn->elseStmt != nullptr) {
            std::string lelse = newLabel();
            // 条件为假时跳到 else，为真则顺序落入 then（省去一次无条件跳转）。
            genCondJump(ifn->condition.get(), lelse, false, 0);
            genStmt(ifn->thenStmt.get());
            scopes = savedScopes;
            emit("j " + lend);
            emitLabel(lelse);
            genStmt(ifn->elseStmt.get());
            emitLabel(lend);
        } else {
            genCondJump(ifn->condition.get(), lend, false, 0);
            genStmt(ifn->thenStmt.get());
            emitLabel(lend);
        }
        // 非常量分支后，两条路径的可变值不能安全合并；保留存储位置，
        // 但丢弃所有可变常量事实，避免把“分支未执行”的旧值带到分支后。
        scopes = std::move(savedScopes);
        if (opt) clearMutableKnownValues();
        clearPureExprCache();
        return;
    }

    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        int condition;
        if (opt && tryEvalConst(wh->condition.get(), condition) && condition == 0) {
            return;
        }
        // 循环可执行零次或多次，且循环体会反复修改局部变量；不能把循环前的
        // 单次常量事实用于条件或下一轮迭代。
        if (opt) clearMutableKnownValues();
        clearPureExprCache();
        // 循环旋转：条件测试置于循环底部，稳态每次迭代仅一条“为真回跳”分支，
        // 省去底部的无条件回跳。首次进入先跳到条件处测试。
        std::string lcond = newLabel();
        std::string lbody = newLabel();
        std::string lend = newLabel();
        emit("j " + lcond);
        emitLabel(lbody);
        loopLabels.push_back({lcond, lend});
        genStmt(wh->body.get());
        loopLabels.pop_back();
        emitLabel(lcond);
        genCondJump(wh->condition.get(), lbody, true, 0);
        emitLabel(lend);
        return;
    }

    if (dynamic_cast<BreakNode*>(node)) {
        emit("j " + loopLabels.back().second);
        return;
    }

    if (dynamic_cast<ContinueNode*>(node)) {
        emit("j " + loopLabels.back().first);
        return;
    }

    if (auto* ret = dynamic_cast<ReturnNode*>(node)) {
        if (ret->retExpr != nullptr) {
            if (auto* call = dynamic_cast<CallNode*>(ret->retExpr.get())) {
                if (tryEmitTailRecursiveCall(call, 0)) return;
            }
        }
        if (ret->retExpr != nullptr) {
            genExpr(ret->retExpr.get(), 0);
        }
        emit("j " + epilogueLabel);
        return;
    }

    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        if (opt && es->expr != nullptr && !containsCall(es->expr.get())) return;
        if (es->expr != nullptr) genExpr(es->expr.get(), 0);
        if (es->expr != nullptr && containsCall(es->expr.get())) clearPureExprCache();
        return;
    }
}

// ---------- 表达式 ----------

bool CodeGen::tryEmitRegBinary(BinaryNode* node, const std::string& dest) {
    if (!opt) return false;

    std::string left;
    std::string right;
    bool leftOk = tryGetReg(node->left.get(), left);
    bool rightOk = tryGetReg(node->right.get(), right);
    const std::string& op = node->op;

    int c;
    if (leftOk && !rightOk && tryEvalConst(node->right.get(), c)) {
        // 12 位立即数可用单条 addi / slti 直达目标。
        if (c >= -2048 && c <= 2047) {
            if (op == "+") { emit("addi " + dest + ", " + left + ", " + std::to_string(c)); return true; }
            if (op == "-" && c > -2048) { emit("addi " + dest + ", " + left + ", " + std::to_string(-c)); return true; }
            if (op == "<") { emit("slti " + dest + ", " + left + ", " + std::to_string(c)); return true; }
        }
        // 其余情况：0 用 zero；已提升的常量用其寄存器。
        if (c == 0) { right = "zero"; rightOk = true; }
        else if (auto it = condConstRegs.find(c); it != condConstRegs.end()) {
            right = it->second;
            rightOk = true;
        }
    } else if (rightOk && !leftOk && tryEvalConst(node->left.get(), c)) {
        if (c == 0) { left = "zero"; leftOk = true; }
        else if (auto it = condConstRegs.find(c); it != condConstRegs.end()) {
            left = it->second;
            leftOk = true;
        }
    }
    if (!leftOk || !rightOk) return false;

    if (op == "+") emit("add " + dest + ", " + left + ", " + right);
    else if (op == "-") emit("sub " + dest + ", " + left + ", " + right);
    else if (op == "*") emit("mul " + dest + ", " + left + ", " + right);
    else if (op == "/") emit("div " + dest + ", " + left + ", " + right);
    else if (op == "%") emit("rem " + dest + ", " + left + ", " + right);
    else if (op == "<") emit("slt " + dest + ", " + left + ", " + right);
    else if (op == ">") emit("slt " + dest + ", " + right + ", " + left);
    else if (op == "<=") { emit("slt " + dest + ", " + right + ", " + left); emit("xori " + dest + ", " + dest + ", 1"); }
    else if (op == ">=") { emit("slt " + dest + ", " + left + ", " + right); emit("xori " + dest + ", " + dest + ", 1"); }
    else if (op == "==") { emit("sub " + dest + ", " + left + ", " + right); emit("seqz " + dest + ", " + dest); }
    else if (op == "!=") { emit("sub " + dest + ", " + left + ", " + right); emit("snez " + dest + ", " + dest); }
    else return false;
    return true;
}

bool CodeGen::exprStructurallyEqual(ExprNode* left, ExprNode* right) const {
    if (left == nullptr || right == nullptr) return left == right;
    if (auto* l = dynamic_cast<LiteralNode*>(left)) {
        auto* r = dynamic_cast<LiteralNode*>(right);
        return r != nullptr && l->value == r->value;
    }
    if (auto* l = dynamic_cast<VarNode*>(left)) {
        auto* r = dynamic_cast<VarNode*>(right);
        return r != nullptr && l->name == r->name;
    }
    if (auto* l = dynamic_cast<UnaryNode*>(left)) {
        auto* r = dynamic_cast<UnaryNode*>(right);
        return r != nullptr && l->op == r->op &&
               exprStructurallyEqual(l->operand.get(), r->operand.get());
    }
    if (auto* l = dynamic_cast<BinaryNode*>(left)) {
        auto* r = dynamic_cast<BinaryNode*>(right);
        return r != nullptr && l->op == r->op &&
               exprStructurallyEqual(l->left.get(), r->left.get()) &&
               exprStructurallyEqual(l->right.get(), r->right.get());
    }
    if (auto* l = dynamic_cast<CallNode*>(left)) {
        auto* r = dynamic_cast<CallNode*>(right);
        if (r == nullptr || l->funcName != r->funcName || l->args.size() != r->args.size()) {
            return false;
        }
        for (size_t i = 0; i < l->args.size(); ++i) {
            if (!exprStructurallyEqual(l->args[i].get(), r->args[i].get())) return false;
        }
        return true;
    }
    return false;
}

bool CodeGen::tryEmitAlgebraicSimplification(BinaryNode* node, int depth) {
    if (!opt) return false;

    int leftConst;
    int rightConst;
    bool leftIsConst = tryEvalConst(node->left.get(), leftConst);
    bool rightIsConst = tryEvalConst(node->right.get(), rightConst);
    const std::string& op = node->op;

    if ((op == "+" && rightIsConst && rightConst == 0) ||
        (op == "-" && rightIsConst && rightConst == 0) ||
        (op == "*" && rightIsConst && rightConst == 1) ||
        (op == "/" && rightIsConst && rightConst == 1)) {
        genExpr(node->left.get(), depth);
        return true;
    }
    if ((op == "+" && leftIsConst && leftConst == 0) ||
        (op == "*" && leftIsConst && leftConst == 1)) {
        genExpr(node->right.get(), depth);
        return true;
    }
    if ((op == "*" && ((leftIsConst && leftConst == 0) ||
                        (rightIsConst && rightConst == 0))) ||
        (op == "%" && rightIsConst && (rightConst == 1 || rightConst == -1))) {
        // 仍要求值非字面量一侧，以保留可能存在的函数调用副作用。
        if (op == "*" && leftIsConst) genExpr(node->right.get(), depth);
        else genExpr(node->left.get(), depth);
        loadImm("a0", 0);
        return true;
    }
    if (op == "*" && ((leftIsConst && leftConst == -1) ||
                      (rightIsConst && rightConst == -1))) {
        if (leftIsConst) genExpr(node->right.get(), depth);
        else genExpr(node->left.get(), depth);
        emit("neg a0, a0");
        return true;
    }

    if (!containsCall(node->left.get()) &&
        exprStructurallyEqual(node->left.get(), node->right.get())) {
        if (op == "-") {
            loadImm("a0", 0);
            return true;
        }
        if (op == "+") {
            genExpr(node->left.get(), depth);
            emit("slli a0, a0, 1");
            return true;
        }
        if (op == "==" || op == "<=" || op == ">=") {
            loadImm("a0", 1);
            return true;
        }
        if (op == "!=" || op == "<" || op == ">") {
            loadImm("a0", 0);
            return true;
        }
    }
    return false;
}

bool CodeGen::tryEmitTailRecursiveCall(CallNode* node, int depth) {
    if (!opt || node->funcName != currentFunctionName ||
        node->args.size() != currentFunctionParams.size() || tailEntryLabel.empty()) {
        return false;
    }

    int n = static_cast<int>(node->args.size());
    std::vector<const VarInfo*> plocs(n);
    for (int i = 0; i < n; ++i) {
        plocs[i] = lookupVar(currentFunctionParams[i]);
        if (plocs[i] == nullptr) throw std::runtime_error("codegen: missing function parameter");
    }

    // 恒等实参（实参就是对应形参本身，且未被内层同名局部遮蔽）无需搬移。
    std::vector<int> moves;
    bool anyArgCall = false;
    for (int i = 0; i < n; ++i) {
        if (containsCall(node->args[i].get())) anyArgCall = true;
        if (auto* v = dynamic_cast<VarNode*>(node->args[i].get())) {
            const VarInfo* av = lookupVar(v->name);
            if (av != nullptr && av->kind == plocs[i]->kind &&
                ((av->kind == VarInfo::Reg && av->reg == plocs[i]->reg) ||
                 (av->kind == VarInfo::Local && av->offset == plocs[i]->offset))) {
                continue;
            }
        }
        moves.push_back(i);
    }

    // 新实参先集中求值再统一写回，避免例如 f(b, a) 覆盖尚待读取的旧形参。
    // 无调用、待搬移实参 <=3 且表达式浅（求值只用 t 寄存器、不落栈）时，
    // 用 t0..t2 中转实现零访存；否则退回栈临时槽的保守路径。
    bool shallow = true;
    for (int i : moves) {
        if (exprMaxDepth(node->args[i].get(), 0) > 2) { shallow = false; break; }
    }
    if (!anyArgCall && shallow && static_cast<int>(moves.size()) <= 3) {
        int stage = 0;
        int evalDepth = static_cast<int>(moves.size());
        for (int i : moves) {
            genExprInto(node->args[i].get(), "t" + std::to_string(stage++), evalDepth);
        }
        stage = 0;
        for (int i : moves) {
            std::string src = "t" + std::to_string(stage++);
            if (plocs[i]->kind == VarInfo::Reg) emit("mv " + plocs[i]->reg + ", " + src);
            else if (plocs[i]->kind == VarInfo::Local) memInsn("sw", src, plocs[i]->offset);
            else throw std::runtime_error("codegen: invalid function parameter location");
        }
    } else {
        for (int i = 0; i < n; ++i) {
            genExpr(node->args[i].get(), depth + i);
            storeTemp(depth + i);
        }
        for (int i = 0; i < n; ++i) {
            loadTemp(depth + i, "a0");
            if (plocs[i]->kind == VarInfo::Reg) emit("mv " + plocs[i]->reg + ", a0");
            else if (plocs[i]->kind == VarInfo::Local) memInsn("sw", "a0", plocs[i]->offset);
            else throw std::runtime_error("codegen: invalid function parameter location");
        }
    }
    emit("j " + tailEntryLabel);
    return true;
}

bool CodeGen::tryEmitOptimizedAssign(AssignNode* node) {
    if (!opt) return false;

    const VarInfo* target = lookupVar(node->name);
    if (target == nullptr || target->kind != VarInfo::Reg) return false;

    auto* bin = dynamic_cast<BinaryNode*>(node->rhs.get());
    if (bin == nullptr || (bin->op != "+" && bin->op != "-")) return false;

    int constant;
    long long delta;
    auto* left = dynamic_cast<VarNode*>(bin->left.get());
    auto* right = dynamic_cast<VarNode*>(bin->right.get());

    if (left != nullptr && left->name == node->name &&
        tryEvalConst(bin->right.get(), constant)) {
        delta = bin->op == "+" ? constant : -static_cast<long long>(constant);
    } else if (bin->op == "+" && right != nullptr && right->name == node->name &&
               tryEvalConst(bin->left.get(), constant)) {
        delta = constant;
    } else {
        return false;
    }

    if (delta < -2048 || delta > 2047) return false;
    if (delta != 0) {
        emit("addi " + target->reg + ", " + target->reg + ", " + std::to_string(delta));
    }
    return true;
}

// 计算二元运算的两个操作数：左值放入某寄存器（名字经 leftReg 返回），右值留在 a0。
// 若右子树不含调用且深度够浅，左值暂存于 t(depth)；否则溢出到栈临时槽、再取回 t6。
void CodeGen::genBinaryOperands(BinaryNode* node, int depth, std::string& leftReg) {
    // 左操作数本身就是寄存器变量时直接引用其寄存器：ToyC 表达式无赋值副作用，
    // 右子树求值只写 a0/t*（内联展开只写其自有的新增局部），寄存器变量保持不变。
    if (opt && tryGetReg(node->left.get(), leftReg)) {
        genExpr(node->right.get(), depth + 1);
        return;
    }
    genExpr(node->left.get(), depth);
    bool useReg = depth < 6 && !containsCall(node->right.get());
    if (useReg) {
        leftReg = "t" + std::to_string(depth);
        emit("mv " + leftReg + ", a0");
    } else {
        storeTemp(depth);
    }
    genExpr(node->right.get(), depth + 1);
    if (!useReg) {
        loadTemp(depth, "t6");
        leftReg = "t6";
    }
}

void CodeGen::genExpr(ExprNode* node, int depth) {
    int cv;
    if (tryEvalConst(node, cv)) {
        loadImm("a0", cv);
        return;
    }

    if (auto* var = dynamic_cast<VarNode*>(node)) {
        const VarInfo* v = lookupVar(var->name);
        if (v == nullptr) throw std::runtime_error("codegen: undefined variable " + var->name);
        if (v->kind == VarInfo::Const) loadImm("a0", v->constVal);
        else if (v->kind == VarInfo::Reg) emit("mv a0, " + v->reg);
        else if (v->kind == VarInfo::Global) { emit("la a0, " + v->label); emit("lw a0, 0(a0)"); }
        else memInsn("lw", "a0", v->offset);
        return;
    }

    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        if (opt) {
            std::string reg;
            if (tryGetReg(un->operand.get(), reg)) {
                if (un->op == "-") {
                    emit("neg a0, " + reg);
                    return;
                }
                if (un->op == "!") {
                    emit("seqz a0, " + reg);
                    return;
                }
            }
        }

        genExpr(un->operand.get(), depth);
        if (un->op == "-") emit("neg a0, a0");
        else if (un->op == "!") emit("seqz a0, a0");
        return;
    }

    if (auto* call = dynamic_cast<CallNode*>(node)) {
        genCall(call, depth);
        return;
    }

    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        if (bin->op == "&&" || bin->op == "||") {
            genLogical(bin, depth);
            return;
        }

        if (tryEmitAlgebraicSimplification(bin, depth)) return;

        if (tryEmitRegBinary(bin)) return;

        if (opt && (bin->op == "+" || bin->op == "-")) {
            std::string reg;
            int constant;
            long long delta;
            if (tryGetReg(bin->left.get(), reg) && tryEvalConst(bin->right.get(), constant)) {
                delta = bin->op == "+" ? constant : -static_cast<long long>(constant);
                if (delta >= -2048 && delta <= 2047) {
                    if (delta == 0) emit("mv a0, " + reg);
                    else emit("addi a0, " + reg + ", " + std::to_string(delta));
                    return;
                }
            } else if (bin->op == "+" && tryGetReg(bin->right.get(), reg) &&
                       tryEvalConst(bin->left.get(), constant) &&
                       constant >= -2048 && constant <= 2047) {
                if (constant == 0) emit("mv a0, " + reg);
                else emit("addi a0, " + reg + ", " + std::to_string(constant));
                return;
            }
        }

        // 强度削弱（-opt）：乘以 2 的幂改为左移（除法不化简：负数截断除法与右移不等价）。
        if (opt && bin->op == "*") {
            int rv;
            if (tryEvalConst(bin->right.get(), rv) && rv > 0 && (rv & (rv - 1)) == 0) {
                int sh = 0;
                while ((1 << sh) < rv) sh++;
                genExpr(bin->left.get(), depth);
                emit("slli a0, a0, " + std::to_string(sh));
                return;
            }
        }

        // 右操作数已在寄存器（变量 / zero / 已提升常量）：左值求到 a0 后单条指令收尾，
        // 免去 t 寄存器/栈临时槽的搬运。加减常量若在 addi 范围也走单指令。
        if (opt && bin->op != "&&" && bin->op != "||") {
            std::string rr;
            bool haveR = tryGetReg(bin->right.get(), rr);
            if (!haveR) {
                int rc;
                if (tryEvalConst(bin->right.get(), rc)) {
                    if (rc == 0) { rr = "zero"; haveR = true; }
                    else if (auto it = condConstRegs.find(rc); it != condConstRegs.end()) {
                        rr = it->second;
                        haveR = true;
                    } else if (rc >= -2047 && rc <= 2047 &&
                               (bin->op == "+" || bin->op == "-")) {
                        genExpr(bin->left.get(), depth);
                        int delta = bin->op == "+" ? rc : -rc;
                        emit("addi a0, a0, " + std::to_string(delta));
                        return;
                    }
                }
            }
            if (haveR) {
                genExpr(bin->left.get(), depth);
                const std::string& op2 = bin->op;
                if (op2 == "+") emit("add a0, a0, " + rr);
                else if (op2 == "-") emit("sub a0, a0, " + rr);
                else if (op2 == "*") emit("mul a0, a0, " + rr);
                else if (op2 == "/") emit("div a0, a0, " + rr);
                else if (op2 == "%") emit("rem a0, a0, " + rr);
                else if (op2 == "<") emit("slt a0, a0, " + rr);
                else if (op2 == ">") emit("slt a0, " + rr + ", a0");
                else if (op2 == "<=") { emit("slt a0, " + rr + ", a0"); emit("xori a0, a0, 1"); }
                else if (op2 == ">=") { emit("slt a0, a0, " + rr); emit("xori a0, a0, 1"); }
                else if (op2 == "==") { emit("sub a0, a0, " + rr); emit("seqz a0, a0"); }
                else { emit("sub a0, a0, " + rr); emit("snez a0, a0"); }
                return;
            }
        }

        std::string L;
        genBinaryOperands(bin, depth, L);

        const std::string& op = bin->op;
        if (op == "+") emit("add a0, " + L + ", a0");
        else if (op == "-") emit("sub a0, " + L + ", a0");
        else if (op == "*") emit("mul a0, " + L + ", a0");
        else if (op == "/") emit("div a0, " + L + ", a0");
        else if (op == "%") emit("rem a0, " + L + ", a0");
        else if (op == "<") emit("slt a0, " + L + ", a0");
        else if (op == ">") emit("slt a0, a0, " + L);
        else if (op == "<=") { emit("slt a0, a0, " + L); emit("xori a0, a0, 1"); }
        else if (op == ">=") { emit("slt a0, " + L + ", a0"); emit("xori a0, a0, 1"); }
        else if (op == "==") { emit("sub a0, " + L + ", a0"); emit("seqz a0, a0"); }
        else if (op == "!=") { emit("sub a0, " + L + ", a0"); emit("snez a0, a0"); }
        return;
    }
}

void CodeGen::genLogical(BinaryNode* node, int depth) {
    std::string lend = newLabel();
    if (node->op == "&&") {
        std::string lfalse = newLabel();
        genExpr(node->left.get(), depth);
        emit("beqz a0, " + lfalse);
        genExpr(node->right.get(), depth);
        emit("snez a0, a0");
        emit("j " + lend);
        emitLabel(lfalse);
        emit("li a0, 0");
        emitLabel(lend);
    } else {
        std::string ltrue = newLabel();
        genExpr(node->left.get(), depth);
        emit("bnez a0, " + ltrue);
        genExpr(node->right.get(), depth);
        emit("snez a0, a0");
        emit("j " + lend);
        emitLabel(ltrue);
        emit("li a0, 1");
        emitLabel(lend);
    }
}

// 条件跳转：对 node 求值后只发一条条件分支到 target；
// jumpWhenTrue 为真表示“条件成立时跳转”，否则“条件不成立时跳转”，另一侧顺序落下。
// 支持 !、&&、|| 的短路，以及关系运算的直接分支（含操作数取反以省去无条件跳转）。
void CodeGen::genCondJump(ExprNode* node, const std::string& target,
                          bool jumpWhenTrue, int depth) {
    int cv;
    if (tryEvalConst(node, cv)) {
        if ((cv != 0) == jumpWhenTrue) emit("j " + target);
        return;
    }

    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        if (un->op == "!") {
            genCondJump(un->operand.get(), target, !jumpWhenTrue, depth);
            return;
        }
    }

    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        if (bin->op == "&&") {
            if (jumpWhenTrue) {
                std::string skip = newLabel();
                genCondJump(bin->left.get(), skip, false, depth);
                genCondJump(bin->right.get(), target, true, depth);
                emitLabel(skip);
            } else {
                genCondJump(bin->left.get(), target, false, depth);
                genCondJump(bin->right.get(), target, false, depth);
            }
            return;
        }
        if (bin->op == "||") {
            if (jumpWhenTrue) {
                genCondJump(bin->left.get(), target, true, depth);
                genCondJump(bin->right.get(), target, true, depth);
            } else {
                std::string skip = newLabel();
                genCondJump(bin->left.get(), skip, true, depth);
                genCondJump(bin->right.get(), target, false, depth);
                emitLabel(skip);
            }
            return;
        }
        const std::string& op = bin->op;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
            // 若“不成立时跳转”，把比较运算取反，仍用单条分支表达。
            std::string eop = op;
            if (!jumpWhenTrue) {
                if (op == "<") eop = ">=";
                else if (op == ">") eop = "<=";
                else if (op == "<=") eop = ">";
                else if (op == ">=") eop = "<";
                else if (op == "==") eop = "!=";
                else eop = "==";
            }
            auto emitBranch = [&](const std::string& lo, const std::string& ro) {
                std::string br;
                if (eop == "<") br = "blt " + lo + ", " + ro + ", ";
                else if (eop == ">") br = "blt " + ro + ", " + lo + ", ";
                else if (eop == "<=") br = "bge " + ro + ", " + lo + ", ";
                else if (eop == ">=") br = "bge " + lo + ", " + ro + ", ";
                else if (eop == "==") br = "beq " + lo + ", " + ro + ", ";
                else br = "bne " + lo + ", " + ro + ", ";
                emit(br + target);
            };

            if (opt) {
                std::string left;
                std::string right;
                bool leftReg = tryGetReg(bin->left.get(), left);
                bool rightReg = tryGetReg(bin->right.get(), right);
                int constant;

                // 常量操作数的寄存器化：0 用 zero；已提升的常量用其 s 寄存器；
                // 其余返回空串，由调用处临时 li 到 t6。
                auto constReg = [&](int c) -> std::string {
                    if (c == 0) return "zero";
                    auto it = condConstRegs.find(c);
                    return it != condConstRegs.end() ? it->second : std::string();
                };

                if (leftReg && rightReg) {
                    emitBranch(left, right);
                    return;
                } else if (leftReg && tryEvalConst(bin->right.get(), constant)) {
                    std::string cr = constReg(constant);
                    if (cr.empty()) { loadImm("t6", constant); cr = "t6"; }
                    emitBranch(left, cr);
                    return;
                } else if (rightReg && tryEvalConst(bin->left.get(), constant)) {
                    std::string cr = constReg(constant);
                    if (cr.empty()) { loadImm("t6", constant); cr = "t6"; }
                    emitBranch(cr, right);
                    return;
                }

                if (leftReg) {
                    genExpr(bin->right.get(), depth);
                    emitBranch(left, "a0");
                    return;
                }
                if (rightReg) {
                    genExpr(bin->left.get(), depth);
                    emitBranch("a0", right);
                    return;
                }
                if (tryEvalConst(bin->right.get(), constant)) {
                    std::string cr = constReg(constant);
                    if (!cr.empty()) {
                        genExpr(bin->left.get(), depth);
                        emitBranch("a0", cr);
                        return;
                    }
                }
                if (tryEvalConst(bin->left.get(), constant)) {
                    std::string cr = constReg(constant);
                    if (!cr.empty()) {
                        genExpr(bin->right.get(), depth);
                        emitBranch(cr, "a0");
                        return;
                    }
                }
            }

            std::string L;
            genBinaryOperands(bin, depth, L);
            emitBranch(L, "a0");
            return;
        }
    }

    genExpr(node, depth);
    emit((jumpWhenTrue ? "bnez a0, " : "beqz a0, ") + target);
}

// ---------- 函数调用 ----------

// 把表达式的值直接求到指定寄存器：常量 li、寄存器变量 mv，其余走 a0 中转。
void CodeGen::genExprInto(ExprNode* node, const std::string& reg, int depth) {
    int cv;
    if (tryEvalConst(node, cv)) {
        loadImm(reg, cv);
        return;
    }
    std::string src;
    if (tryGetReg(node, src)) {
        if (src != reg) emit("mv " + reg + ", " + src);
        return;
    }
    // x = y op z（寄存器/立即数操作数）单条指令直达目标寄存器。
    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        if (bin->op != "&&" && bin->op != "||") {
            if (tryEmitRegBinary(bin, reg)) return;
            // 右操作数在寄存器（变量 / zero / 已提升常量）：左值经 a0 后直达目标。
            std::string rr;
            bool haveR = tryGetReg(bin->right.get(), rr);
            int rc;
            if (!haveR && tryEvalConst(bin->right.get(), rc)) {
                if (rc == 0) { rr = "zero"; haveR = true; }
                else if (auto it = condConstRegs.find(rc); it != condConstRegs.end()) {
                    rr = it->second;
                    haveR = true;
                }
            }
            if (haveR) {
                genExpr(bin->left.get(), depth);
                const std::string& op = bin->op;
                if (op == "+") { emit("add " + reg + ", a0, " + rr); return; }
                if (op == "-") { emit("sub " + reg + ", a0, " + rr); return; }
                if (op == "*") { emit("mul " + reg + ", a0, " + rr); return; }
                if (op == "/") { emit("div " + reg + ", a0, " + rr); return; }
                if (op == "%") { emit("rem " + reg + ", a0, " + rr); return; }
                if (op == "<") { emit("slt " + reg + ", a0, " + rr); return; }
                if (op == ">") { emit("slt " + reg + ", " + rr + ", a0"); return; }
            }
        }
    }
    genExpr(node, depth);
    if (reg != "a0") emit("mv " + reg + ", a0");
}

// 小叶子函数在调用点展开：实参直接写入形参的存储位置（等同调用者新增局部，
// 已计入帧预分析），函数体在调用者栈帧上就地生成。生成期间：
//   - 作用域栈只保留全局层再叠加形参层，杜绝调用者局部被被内联体误解析；
//   - 被内联体的 return 改写为跳到本次展开的结束标签，返回值仍留在 a0；
//   - 其临时槽经 tempBias 整体平移，避开调用者此刻在用的槽位。
void CodeGen::genInlineCall(CallNode* node, int depth) {
    FuncDefNode* callee = funcDefs.at(node->funcName);
    int n = static_cast<int>(node->args.size());

    clearPureExprCache();
    int savedCursor = localCursor;
    std::vector<VarInfo> paramLocs;
    paramLocs.reserve(n);
    for (int i = 0; i < n; ++i) paramLocs.push_back(assignLocalLocation());

    // 实参仍在调用者作用域中求值（形参尚未进作用域），逐个写入形参存储。
    for (int i = 0; i < n; ++i) {
        if (paramLocs[i].kind == VarInfo::Reg) {
            genExprInto(node->args[i].get(), paramLocs[i].reg, depth);
        } else {
            genExpr(node->args[i].get(), depth);
            memInsn("sw", "a0", paramLocs[i].offset);
        }
    }

    auto savedScopes = std::move(scopes);
    scopes.clear();
    scopes.push_back(savedScopes[0]);  // 仅保留全局层
    pushScope();
    for (int i = 0; i < n; ++i) {
        scopes.back()[callee->params[i]] = paramLocs[i];
    }

    std::string savedEpilogue = epilogueLabel;
    int savedBias = tempBias;
    epilogueLabel = newLabel();
    tempBias += depth;

    genBlock(callee->body.get(), false);
    emitLabel(epilogueLabel);

    tempBias = savedBias;
    epilogueLabel = savedEpilogue;
    popScope();
    scopes = std::move(savedScopes);
    localCursor = savedCursor;
    clearPureExprCache();
}

void CodeGen::genCall(CallNode* node, int depth) {
    if (opt && inlinableFuncs.count(node->funcName)) {
        genInlineCall(node, depth);
        return;
    }

    clearPureExprCache();
    int n = static_cast<int>(node->args.size());

    if (opt && n == 1) {
        genExpr(node->args[0].get(), depth);
        emit("call " + node->funcName);
        return;
    }

    // 实参从左到右求值并溢出到栈临时槽（跨后续实参/调用安全）。
    for (int i = 0; i < n; ++i) {
        genExpr(node->args[i].get(), depth + i);
        storeTemp(depth + i);
    }

    int extra = n > 8 ? n - 8 : 0;
    int space = 0;
    if (extra > 0) {
        space = (extra * 4 + 15) & ~15;
        emit("addi sp, sp, -" + std::to_string(space));
        for (int j = 0; j < extra; ++j) {
            loadTemp(depth + 8 + j, "t0");
            emit("sw t0, " + std::to_string(4 * j) + "(sp)");
        }
    }

    int inReg = n < 8 ? n : 8;
    for (int i = 0; i < inReg; ++i) {
        loadTemp(depth + i, "a" + std::to_string(i));
    }

    emit("call " + node->funcName);

    if (extra > 0) {
        emit("addi sp, sp, " + std::to_string(space));
    }
}

// ---------- 窥孔优化 ----------

void CodeGen::peephole() {
    if (!opt) return;

    std::vector<std::string> out;
    out.reserve(cur.size());
    for (size_t i = 0; i < cur.size(); ++i) {
        const std::string& line = cur[i];

        // 删除 mv x, x。
        if (line.rfind("\tmv ", 0) == 0) {
            std::string rest = line.substr(4);
            size_t comma = rest.find(", ");
            if (comma != std::string::npos && rest.substr(0, comma) == rest.substr(comma + 2)) {
                continue;
            }
        }

        // 删除紧跟目标标签的无条件跳转：j L 后紧接 L:。
        if (line.rfind("\tj ", 0) == 0 && i + 1 < cur.size()) {
            if (cur[i + 1] == line.substr(3) + ":") continue;
        }

        // 删除紧跟同址存储的冗余载入：sw a0, X 后紧接 lw a0, X。
        if (!out.empty() && line.rfind("\tlw a0, ", 0) == 0) {
            const std::string& prev = out.back();
            if (prev.rfind("\tsw a0, ", 0) == 0 &&
                prev.substr(8) == line.substr(8)) {
                continue;
            }
        }

        out.push_back(line);
    }
    cur.swap(out);
}
