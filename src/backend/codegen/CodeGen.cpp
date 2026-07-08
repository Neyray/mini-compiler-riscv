#include "CodeGen.h"
#include <stdexcept>

CodeGen::CodeGen(bool optimize)
    : opt(optimize), labelCounter(0), totalLocals(0), localCursor(0) {}

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

// 以 s0 为基址的 load/store，处理超出 12 位立即数范围的偏移。
void CodeGen::memInsn(const std::string& op, const std::string& reg, int offset) {
    if (offset >= -2048 && offset <= 2047) {
        emit(op + " " + reg + ", " + std::to_string(offset) + "(s0)");
    } else {
        emit("li t1, " + std::to_string(offset));
        emit("add t1, s0, t1");
        emit(op + " " + reg + ", 0(t1)");
    }
}

int CodeGen::localOffset(int slot) const {
    // ra 在 s0-4、旧 s0 在 s0-8，局部区从 s0-12 向下。
    return -12 - 4 * slot;
}

int CodeGen::tempOffset(int slot) const {
    return -12 - 4 * totalLocals - 4 * slot;
}

void CodeGen::storeTemp(int slot) {
    memInsn("sw", "a0", tempOffset(slot));
}

void CodeGen::loadTemp(int slot, const std::string& reg) {
    memInsn("lw", reg, tempOffset(slot));
}

// ---------- 帧尺寸预计算 ----------

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

// 表达式求值所需的临时槽数（左操作数溢出深度）。
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
            return l > r ? l : r;  // 短路求值不保留左值
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
        // 短路运算符：右侧可能不需要求值，但既然要折叠为常量，两侧都需常量。
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
    return false; // 函数调用不是常量
}

// ---------- 顶层 ----------

void CodeGen::generate(CompUnitNode* root, std::ostream& out) {
    scopes.clear();
    pushScope(); // 全局作用域

    // 先登记全部全局声明（变量/常量），再生成函数，允许函数引用其前的全局。
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
                // 普通全局变量需要静态初值；语义已保证可编译期确定。
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
    int temps = stmtTempNeed(func->body.get());
    localCursor = 0;

    // 帧：8(ra,s0) + 局部区 + 临时区，向上取整到 16。
    int frame = 8 + 4 * totalLocals + 4 * temps;
    frame = (frame + 15) & ~15;

    epilogueLabel = newLabel();

    emit(".globl " + func->name);
    emitLabel(func->name);

    // 序言：分配帧，保存 ra / 旧 s0，建立 s0（帧顶）。
    emit("li t0, " + std::to_string(frame));
    emit("sub sp, sp, t0");
    emit("add t0, sp, t0");
    emit("sw ra, -4(t0)");
    emit("sw s0, -8(t0)");
    emit("mv s0, t0");

    pushScope(); // 函数体作用域（形参与最外层局部共用）

    // 形参落栈：前 8 个来自 a0..a7，其余来自调用者栈（s0 正偏移）。
    for (int i = 0; i < nparams; ++i) {
        int slot = localCursor++;
        VarInfo info;
        info.kind = VarInfo::Local;
        info.offset = localOffset(slot);
        scopes.back()[func->params[i]] = info;

        if (i < 8) {
            memInsn("sw", "a" + std::to_string(i), info.offset);
        } else {
            int inOff = 4 * (i - 8);
            if (inOff >= -2048 && inOff <= 2047) {
                emit("lw t0, " + std::to_string(inOff) + "(s0)");
            } else {
                emit("li t1, " + std::to_string(inOff));
                emit("add t1, s0, t1");
                emit("lw t0, 0(t1)");
            }
            memInsn("sw", "t0", info.offset);
        }
    }

    genBlock(func->body.get(), false);

    // 尾声。
    emitLabel(epilogueLabel);
    emit("lw ra, -4(s0)");
    emit("lw t0, -8(s0)");
    emit("mv sp, s0");
    emit("mv s0, t0");
    emit("ret");

    popScope();

    if (opt) peephole();

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
            // 理论上 const 初值必为常量；保险起见退化为局部变量。
        }
        int slot = localCursor++;
        VarInfo info;
        info.kind = VarInfo::Local;
        info.offset = localOffset(slot);
        genExpr(decl->initExpr.get(), 0);
        memInsn("sw", "a0", info.offset);
        scopes.back()[decl->name] = info;
        return;
    }

    if (auto* asn = dynamic_cast<AssignNode*>(node)) {
        genExpr(asn->rhs.get(), 0);
        const VarInfo* v = lookupVar(asn->name);
        if (v == nullptr) throw std::runtime_error("codegen: undefined variable " + asn->name);
        if (v->kind == VarInfo::Global) {
            emit("la t1, " + v->label);
            emit("sw a0, 0(t1)");
        } else if (v->kind == VarInfo::Local) {
            memInsn("sw", "a0", v->offset);
        }
        // Const 不能作为左值，语义已排除。
        return;
    }

    if (auto* ifn = dynamic_cast<IfNode*>(node)) {
        std::string lthen = newLabel();
        std::string lend = newLabel();
        if (ifn->elseStmt != nullptr) {
            std::string lelse = newLabel();
            genCond(ifn->condition.get(), lthen, lelse, 0);
            emitLabel(lthen);
            genStmt(ifn->thenStmt.get());
            emit("j " + lend);
            emitLabel(lelse);
            genStmt(ifn->elseStmt.get());
            emitLabel(lend);
        } else {
            genCond(ifn->condition.get(), lthen, lend, 0);
            emitLabel(lthen);
            genStmt(ifn->thenStmt.get());
            emitLabel(lend);
        }
        return;
    }

    if (auto* wh = dynamic_cast<WhileNode*>(node)) {
        std::string lcond = newLabel();
        std::string lbody = newLabel();
        std::string lend = newLabel();
        emitLabel(lcond);
        genCond(wh->condition.get(), lbody, lend, 0);
        emitLabel(lbody);
        loopLabels.push_back({lcond, lend});
        genStmt(wh->body.get());
        loopLabels.pop_back();
        emit("j " + lcond);
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

void CodeGen::genExpr(ExprNode* node, int depth) {
    // 常量折叠：整棵子树可编译期确定时直接 li。
    int cv;
    if (tryEvalConst(node, cv)) {
        loadImm("a0", cv);
        return;
    }

    if (auto* var = dynamic_cast<VarNode*>(node)) {
        const VarInfo* v = lookupVar(var->name);
        if (v == nullptr) throw std::runtime_error("codegen: undefined variable " + var->name);
        if (v->kind == VarInfo::Const) {
            loadImm("a0", v->constVal);
        } else if (v->kind == VarInfo::Global) {
            emit("la a0, " + v->label);
            emit("lw a0, 0(a0)");
        } else {
            memInsn("lw", "a0", v->offset);
        }
        return;
    }

    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        genExpr(un->operand.get(), depth);
        if (un->op == "-") emit("neg a0, a0");
        else if (un->op == "!") emit("seqz a0, a0");
        // "+" 无操作
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

        // 强度削弱（-opt）：乘以 2 的幂改为左移。
        // 除法不做此化简：有符号截断除法与算术右移对负数不等价。
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

        genExpr(bin->left.get(), depth);
        storeTemp(depth);
        genExpr(bin->right.get(), depth + 1);
        loadTemp(depth, "t0"); // t0 = 左值，a0 = 右值

        const std::string& op = bin->op;
        if (op == "+") emit("add a0, t0, a0");
        else if (op == "-") emit("sub a0, t0, a0");
        else if (op == "*") emit("mul a0, t0, a0");
        else if (op == "/") emit("div a0, t0, a0");
        else if (op == "%") emit("rem a0, t0, a0");
        else if (op == "<") emit("slt a0, t0, a0");
        else if (op == ">") emit("slt a0, a0, t0");
        else if (op == "<=") { emit("slt a0, a0, t0"); emit("xori a0, a0, 1"); }
        else if (op == ">=") { emit("slt a0, t0, a0"); emit("xori a0, a0, 1"); }
        else if (op == "==") { emit("sub a0, t0, a0"); emit("seqz a0, a0"); }
        else if (op == "!=") { emit("sub a0, t0, a0"); emit("snez a0, a0"); }
        return;
    }
}

// 短路逻辑，结果规约为 0/1 落在 a0。
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
    } else { // ||
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

// 条件跳转：cond 为真跳 labelTrue，否则跳 labelFalse。
void CodeGen::genCond(ExprNode* node, const std::string& labelTrue,
                      const std::string& labelFalse, int depth) {
    int cv;
    if (tryEvalConst(node, cv)) {
        emit("j " + (cv != 0 ? labelTrue : labelFalse));
        return;
    }

    if (auto* un = dynamic_cast<UnaryNode*>(node)) {
        if (un->op == "!") {
            genCond(un->operand.get(), labelFalse, labelTrue, depth);
            return;
        }
    }

    if (auto* bin = dynamic_cast<BinaryNode*>(node)) {
        if (bin->op == "&&") {
            std::string mid = newLabel();
            genCond(bin->left.get(), mid, labelFalse, depth);
            emitLabel(mid);
            genCond(bin->right.get(), labelTrue, labelFalse, depth);
            return;
        }
        if (bin->op == "||") {
            std::string mid = newLabel();
            genCond(bin->left.get(), labelTrue, mid, depth);
            emitLabel(mid);
            genCond(bin->right.get(), labelTrue, labelFalse, depth);
            return;
        }
        const std::string& op = bin->op;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
            genExpr(bin->left.get(), depth);
            storeTemp(depth);
            genExpr(bin->right.get(), depth + 1);
            loadTemp(depth, "t0"); // t0 = 左，a0 = 右
            std::string br;
            if (op == "<") br = "blt t0, a0, ";
            else if (op == ">") br = "blt a0, t0, ";
            else if (op == "<=") br = "bge a0, t0, ";
            else if (op == ">=") br = "bge t0, a0, ";
            else if (op == "==") br = "beq t0, a0, ";
            else br = "bne t0, a0, ";
            emit(br + labelTrue);
            emit("j " + labelFalse);
            return;
        }
    }

    // 一般表达式：算到 a0，非零则真。
    genExpr(node, depth);
    emit("bnez a0, " + labelTrue);
    emit("j " + labelFalse);
}

// ---------- 函数调用 ----------

void CodeGen::genCall(CallNode* node, int depth) {
    int n = static_cast<int>(node->args.size());

    // 依次求值实参并溢出到临时槽 depth..depth+n-1。
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
    // 返回值在 a0。
}

// ---------- 窥孔优化 ----------

void CodeGen::peephole() {
    std::vector<std::string> out;
    out.reserve(cur.size());
    for (size_t i = 0; i < cur.size(); ++i) {
        const std::string& line = cur[i];

        // 删除 mv x, x。
        if (line == "\tmv a0, a0" || line == "\tmv s0, s0" || line == "\tmv sp, sp") {
            continue;
        }

        // 删除紧跟目标标签的无条件跳转：  j L / L:
        if (line.rfind("\tj ", 0) == 0 && i + 1 < cur.size()) {
            std::string target = line.substr(3);
            if (cur[i + 1] == target + ":") {
                continue;
            }
        }

        out.push_back(line);
    }
    cur.swap(out);
}
