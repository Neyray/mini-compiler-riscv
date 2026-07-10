#include "CodeGen.h"
#include <stdexcept>

CodeGen::CodeGen(bool optimize)
    : opt(optimize), labelCounter(0), totalLocals(0), regLocals(0),
      stackLocalBase(0), tempBaseOffset(0), localCursor(0), funcHasCall(false),
      aRegLeaf(false) {}

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
    return tempBaseOffset - 4 * slot;
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
        return decl->isConst ? 0 : 1;
    }
    if (auto* block = dynamic_cast<BlockNode*>(node)) {
        int n = 0;
        for (const auto& s : block->stmts) n += countLocalDecls(s.get());
        return n;
    }
    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        return countLocalDecls(ifn->thenStmt.get()) + countLocalDecls(ifn->elseStmt.get());
    }
    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        return countLocalDecls(wh->body.get());
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
        int best = n;
        for (int i = 0; i < n; ++i) {
            int need = i + exprTempNeed(call->args[i].get());
            if (need > best) best = need;
        }
        return best;
    }
    return 0;
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

    stackLocalBase = -12 - 4 * regLocals;
    tempBaseOffset = stackLocalBase - 4 * stackLocals;

    int frameWords = 2 + regLocals + stackLocals + temps;
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
    for (int i = 0; i < regLocals; ++i) {
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

    genBlock(func->body.get(), false);

    emitLabel(epilogueLabel);
    if (funcHasCall) emit("lw ra, -4(s0)");
    for (int i = 0; i < regLocals; ++i) {
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
    if (createScope) pushScope();
    for (const auto& s : node->stmts) genStmt(s.get());
    if (createScope) popScope();
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
                return;
            }
        }
        VarInfo info = assignLocalLocation();
        genExpr(decl->initExpr.get(), 0);
        if (info.kind == VarInfo::Reg) emit("mv " + info.reg + ", a0");
        else memInsn("sw", "a0", info.offset);
        scopes.back()[decl->name] = info;
        return;
    }

    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        if (tryEmitOptimizedAssign(asn)) return;

        genExpr(asn->rhs.get(), 0);
        const VarInfo* v = lookupVar(asn->name);
        if (v == nullptr) throw std::runtime_error("codegen: undefined variable " + asn->name);
        if (v->kind == VarInfo::Global) {
            emit("la t6, " + v->label);
            emit("sw a0, 0(t6)");
        } else if (v->kind == VarInfo::Reg) {
            emit("mv " + v->reg + ", a0");
        } else if (v->kind == VarInfo::Local) {
            memInsn("sw", "a0", v->offset);
        }
        return;
    }

    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        std::string lend = newLabel();
        if (ifn->elseStmt != nullptr) {
            std::string lelse = newLabel();
            // 条件为假时跳到 else，为真则顺序落入 then（省去一次无条件跳转）。
            genCondJump(ifn->condition.get(), lelse, false, 0);
            genStmt(ifn->thenStmt.get());
            emit("j " + lend);
            emitLabel(lelse);
            genStmt(ifn->elseStmt.get());
            emitLabel(lend);
        } else {
            genCondJump(ifn->condition.get(), lend, false, 0);
            genStmt(ifn->thenStmt.get());
            emitLabel(lend);
        }
        return;
    }

    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
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
            genExpr(ret->retExpr.get(), 0);
        }
        emit("j " + epilogueLabel);
        return;
    }

    if (auto* es = dynamic_cast<ExprStmtNode*>(node)) {
        if (es->expr != nullptr) genExpr(es->expr.get(), 0);
        return;
    }
}

// ---------- 表达式 ----------

bool CodeGen::tryEmitRegBinary(BinaryNode* node) {
    if (!opt) return false;

    std::string left;
    std::string right;
    if (!tryGetReg(node->left.get(), left) || !tryGetReg(node->right.get(), right)) {
        return false;
    }

    const std::string& op = node->op;
    if (op == "+") emit("add a0, " + left + ", " + right);
    else if (op == "-") emit("sub a0, " + left + ", " + right);
    else if (op == "*") emit("mul a0, " + left + ", " + right);
    else if (op == "/") emit("div a0, " + left + ", " + right);
    else if (op == "%") emit("rem a0, " + left + ", " + right);
    else if (op == "<") emit("slt a0, " + left + ", " + right);
    else if (op == ">") emit("slt a0, " + right + ", " + left);
    else if (op == "<=") { emit("slt a0, " + right + ", " + left); emit("xori a0, a0, 1"); }
    else if (op == ">=") { emit("slt a0, " + left + ", " + right); emit("xori a0, a0, 1"); }
    else if (op == "==") { emit("sub a0, " + left + ", " + right); emit("seqz a0, a0"); }
    else if (op == "!=") { emit("sub a0, " + left + ", " + right); emit("snez a0, a0"); }
    else return false;
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

                if (leftReg && rightReg) {
                    emitBranch(left, right);
                    return;
                } else if (leftReg && tryEvalConst(bin->right.get(), constant)) {
                    if (constant == 0) emitBranch(left, "zero");
                    else {
                        loadImm("t6", constant);
                        emitBranch(left, "t6");
                    }
                    return;
                } else if (rightReg && tryEvalConst(bin->left.get(), constant)) {
                    if (constant == 0) emitBranch("zero", right);
                    else {
                        loadImm("t6", constant);
                        emitBranch("t6", right);
                    }
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
                if (tryEvalConst(bin->right.get(), constant) && constant == 0) {
                    genExpr(bin->left.get(), depth);
                    emitBranch("a0", "zero");
                    return;
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

void CodeGen::genCall(CallNode* node, int depth) {
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
