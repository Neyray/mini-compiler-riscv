#include "Optimizer.h"
#include <algorithm>

// ---------- 通用工具 ----------

int Optimizer::wrap32(long long v) {
    return static_cast<int>(static_cast<unsigned int>(v & 0xffffffffLL));
}

bool Optimizer::isLit(ExprNode* e, int& v) {
    if (auto* lit = dynamic_cast<LiteralNode*>(e)) {
        v = lit->value;
        return true;
    }
    return false;
}

bool Optimizer::exprHasCall(ExprNode* e) {
    if (e == nullptr) return false;
    if (dynamic_cast<CallNode*>(e)) return true;
    if (auto* un = dynamic_cast<UnaryNode*>(e)) return exprHasCall(un->operand.get());
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        return exprHasCall(bin->left.get()) || exprHasCall(bin->right.get());
    }
    return false;
}

bool Optimizer::stmtHasCall(StmtNode* s) {
    if (s == nullptr) return false;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        for (const auto& c : block->stmts) if (stmtHasCall(c.get())) return true;
        return false;
    }
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) return exprHasCall(d->initExpr.get());
    if (auto* a = dynamic_cast<AssignNode*>(s)) return exprHasCall(a->rhs.get());
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        return exprHasCall(i->condition.get()) || stmtHasCall(i->thenStmt.get()) ||
               stmtHasCall(i->elseStmt.get());
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        return exprHasCall(w->condition.get()) || stmtHasCall(w->body.get());
    }
    if (auto* r = dynamic_cast<ReturnNode*>(s)) return exprHasCall(r->retExpr.get());
    if (auto* e = dynamic_cast<ExprStmtNode*>(s)) return exprHasCall(e->expr.get());
    return false;
}

void Optimizer::collectVarNames(ExprNode* e, std::unordered_set<std::string>& out) {
    if (e == nullptr) return;
    if (auto* v = dynamic_cast<VarNode*>(e)) { out.insert(v->name); return; }
    if (auto* un = dynamic_cast<UnaryNode*>(e)) { collectVarNames(un->operand.get(), out); return; }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        collectVarNames(bin->left.get(), out);
        collectVarNames(bin->right.get(), out);
        return;
    }
    if (auto* call = dynamic_cast<CallNode*>(e)) {
        for (const auto& a : call->args) collectVarNames(a.get(), out);
    }
}

void Optimizer::collectAssignedNames(StmtNode* s, std::unordered_set<std::string>& out) {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        for (const auto& c : block->stmts) collectAssignedNames(c.get(), out);
        return;
    }
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) { out.insert(d->name); return; }
    if (auto* a = dynamic_cast<AssignNode*>(s)) { out.insert(a->name); return; }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        collectAssignedNames(i->thenStmt.get(), out);
        collectAssignedNames(i->elseStmt.get(), out);
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        collectAssignedNames(w->body.get(), out);
        return;
    }
}

std::string Optimizer::exprKey(ExprNode* e) {
    if (e == nullptr) return "";
    int v;
    if (isLit(e, v)) return "#" + std::to_string(v);
    if (auto* var = dynamic_cast<VarNode*>(e)) return "$" + var->name;
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        return "(" + un->op + " " + exprKey(un->operand.get()) + ")";
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        return "(" + bin->op + " " + exprKey(bin->left.get()) + " " + exprKey(bin->right.get()) + ")";
    }
    if (auto* call = dynamic_cast<CallNode*>(e)) {
        std::string k = "(call " + call->funcName;
        for (const auto& a : call->args) k += " " + exprKey(a.get());
        return k + ")";
    }
    return "?";
}

std::unique_ptr<ExprNode> Optimizer::cloneExpr(ExprNode* e) {
    if (e == nullptr) return nullptr;
    int v;
    if (isLit(e, v)) return std::make_unique<LiteralNode>(v);
    if (auto* var = dynamic_cast<VarNode*>(e)) return std::make_unique<VarNode>(var->name);
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        return std::make_unique<UnaryNode>(un->op, cloneExpr(un->operand.get()));
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        return std::make_unique<BinaryNode>(bin->op, cloneExpr(bin->left.get()),
                                            cloneExpr(bin->right.get()));
    }
    if (auto* call = dynamic_cast<CallNode*>(e)) {
        std::vector<std::unique_ptr<ExprNode>> args;
        for (const auto& a : call->args) args.push_back(cloneExpr(a.get()));
        return std::make_unique<CallNode>(call->funcName, std::move(args));
    }
    return nullptr;
}

// ---------- 归一化 ----------

void Optimizer::normalizeStmt(std::unique_ptr<StmtNode>& s) {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s.get())) {
        for (auto& c : block->stmts) normalizeStmt(c);
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(s.get())) {
        auto wrap = [](std::unique_ptr<StmtNode>& t) {
            if (t != nullptr && dynamic_cast<BlockNode*>(t.get()) == nullptr) {
                auto b = std::make_unique<BlockNode>();
                b->stmts.push_back(std::move(t));
                t = std::move(b);
            }
        };
        wrap(i->thenStmt);
        wrap(i->elseStmt);
        normalizeStmt(i->thenStmt);
        normalizeStmt(i->elseStmt);
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s.get())) {
        if (dynamic_cast<BlockNode*>(w->body.get()) == nullptr) {
            auto b = std::make_unique<BlockNode>();
            b->stmts.push_back(std::move(w->body));
            w->body = std::move(b);
        }
        normalizeStmt(w->body);
        return;
    }
}

// ---------- 全局常量传播 ----------

namespace {
int wrapG(long long v) {
    return static_cast<int>(static_cast<unsigned int>(v & 0xffffffffLL));
}
// 仅由字面量与已知常量全局构成的表达式求值。
bool evalConstG(ExprNode* e, const std::unordered_map<std::string, int>& cg, int& out) {
    if (e == nullptr) return false;
    if (auto* lit = dynamic_cast<LiteralNode*>(e)) { out = lit->value; return true; }
    if (auto* var = dynamic_cast<VarNode*>(e)) {
        auto it = cg.find(var->name);
        if (it == cg.end()) return false;
        out = it->second;
        return true;
    }
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        int v;
        if (!evalConstG(un->operand.get(), cg, v)) return false;
        if (un->op == "+") { out = v; return true; }
        if (un->op == "-") { out = wrapG(-(long long)v); return true; }
        if (un->op == "!") { out = (v == 0) ? 1 : 0; return true; }
        return false;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        int l, r;
        if (!evalConstG(bin->left.get(), cg, l)) return false;
        if (bin->op == "&&") {
            if (l == 0) { out = 0; return true; }
            if (!evalConstG(bin->right.get(), cg, r)) return false;
            out = (r != 0) ? 1 : 0;
            return true;
        }
        if (bin->op == "||") {
            if (l != 0) { out = 1; return true; }
            if (!evalConstG(bin->right.get(), cg, r)) return false;
            out = (r != 0) ? 1 : 0;
            return true;
        }
        if (!evalConstG(bin->right.get(), cg, r)) return false;
        long long L = l, R = r;
        if (bin->op == "+") { out = wrapG(L + R); return true; }
        if (bin->op == "-") { out = wrapG(L - R); return true; }
        if (bin->op == "*") { out = wrapG(L * R); return true; }
        if (bin->op == "/") { if (r == 0) return false; out = wrapG(L / R); return true; }
        if (bin->op == "%") { if (r == 0) return false; out = wrapG(L % R); return true; }
        if (bin->op == "<")  { out = l <  r; return true; }
        if (bin->op == ">")  { out = l >  r; return true; }
        if (bin->op == "<=") { out = l <= r; return true; }
        if (bin->op == ">=") { out = l >= r; return true; }
        if (bin->op == "==") { out = l == r; return true; }
        if (bin->op == "!=") { out = l != r; return true; }
    }
    return false;
}
}  // namespace

void Optimizer::computeConstGlobals(CompUnitNode* root) {
    // 任一函数中出现过 “name = ...” 的名字视为可变（按名保守处理遮蔽）。
    std::unordered_set<std::string> assigned;
    for (const auto& item : root->items) {
        if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) {
            collectAssignedNames(f->body.get(), assigned);
        }
    }
    // 迭代折叠全局初始化链（前面的全局常量喂给后面的初始化式）。
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& item : root->items) {
            auto* d = dynamic_cast<DeclStmtNode*>(item.get());
            if (d == nullptr || constGlobals.count(d->name)) continue;
            if (!d->isConst && assigned.count(d->name)) continue;
            int v;
            if (evalConstG(d->initExpr.get(), constGlobals, v)) {
                constGlobals[d->name] = v;
                d->initExpr = std::make_unique<LiteralNode>(v);
                grew = true;
            }
        }
    }
}

// ---------- env / CSE 基础操作 ----------

bool Optimizer::envIsLocal(const std::string& name) const {
    for (auto it = env.rbegin(); it != env.rend(); ++it) {
        if (it->count(name)) return true;
    }
    return false;
}

Optimizer::Val* Optimizer::envLookup(const std::string& name) {
    for (auto it = env.rbegin(); it != env.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

void Optimizer::envSet(const std::string& name, const Val& v) {
    for (auto it = env.rbegin(); it != env.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) { f->second = v; return; }
    }
}

void Optimizer::envKillCopiesOf(const std::string& name) {
    for (auto& scope : env) {
        for (auto& [k, v] : scope) {
            if (v.kind == 2 && v.src == name) v = Val{};
        }
    }
}

void Optimizer::cseKillVar(const std::string& name) {
    for (auto it = cse.begin(); it != cse.end();) {
        if (it->second.var == name || it->second.deps.count(name)) it = cse.erase(it);
        else ++it;
    }
}

void Optimizer::envIntersect(Env& a, const Env& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        for (auto& [k, v] : a[i]) {
            auto it = b[i].find(k);
            if (it == b[i].end() || it->second.kind != v.kind ||
                (v.kind == 1 && it->second.c != v.c) ||
                (v.kind == 2 && it->second.src != v.src)) {
                v = Val{};
            }
        }
    }
}

Optimizer::Val Optimizer::valOfExpr(ExprNode* e) {
    Val v;
    int c;
    if (isLit(e, c)) { v.kind = 1; v.c = c; return v; }
    if (auto* var = dynamic_cast<VarNode*>(e)) {
        if (envIsLocal(var->name)) { v.kind = 2; v.src = var->name; }
    }
    return v;
}

// ---------- 常量/复写传播 + 折叠 + 代数化简 + CSE ----------

void Optimizer::foldFunc(FuncDefNode* f) {
    env.clear();
    env.push_back({});
    for (const auto& p : f->params) env.back()[p] = Val{};
    cse.clear();
    std::unique_ptr<StmtNode> body(f->body.release());
    foldStmt(body);
    // 函数体永远保持 Block 结构。
    if (auto* b = dynamic_cast<BlockNode*>(body.get())) {
        f->body.reset(static_cast<BlockNode*>(body.release()));
        (void)b;
    } else {
        auto nb = std::make_unique<BlockNode>();
        if (body != nullptr) nb->stmts.push_back(std::move(body));
        f->body = std::move(nb);
    }
    env.clear();
    cse.clear();
}

void Optimizer::foldStmt(std::unique_ptr<StmtNode>& s) {
    if (s == nullptr) return;

    if (auto* block = dynamic_cast<BlockNode*>(s.get())) {
        env.push_back({});
        for (auto& c : block->stmts) foldStmt(c);
        block->stmts.erase(std::remove(block->stmts.begin(), block->stmts.end(), nullptr),
                           block->stmts.end());
        env.pop_back();
        cse.clear();  // 作用域退出后按名记录的等价关系不再可靠
        return;
    }

    if (auto* d = dynamic_cast<DeclStmtNode*>(s.get())) {
        foldExpr(d->initExpr);
        // CSE：右值与此前某局部持有的纯表达式相同 → 直接复用该变量。
        std::string key = exprKey(d->initExpr.get());
        auto hit = cse.find(key);
        if (hit != cse.end() && hit->second.var != d->name) {
            d->initExpr = std::make_unique<VarNode>(hit->second.var);
            changed = true;
        }
        envKillCopiesOf(d->name);   // 名字捕获：内层重名声明使旧复写失效
        cseKillVar(d->name);
        Val dv = valOfExpr(d->initExpr.get());
        if (dv.kind == 2 && dv.src == d->name) dv = Val{};   // 防自复写
        env.back()[d->name] = dv;
        // 记录 CSE：纯表达式、依赖均为局部、目标不在依赖中。
        if (auto* bin = dynamic_cast<BinaryNode*>(d->initExpr.get())) {
            (void)bin;
            if (!exprHasCall(d->initExpr.get())) {
                std::unordered_set<std::string> deps;
                collectVarNames(d->initExpr.get(), deps);
                bool ok = !deps.empty() && !deps.count(d->name);
                for (const auto& n : deps) if (!envIsLocal(n)) { ok = false; break; }
                if (ok) cse[exprKey(d->initExpr.get())] = {d->name, deps};
            }
        }
        return;
    }

    if (auto* a = dynamic_cast<AssignNode*>(s.get())) {
        foldExpr(a->rhs);
        std::string key = exprKey(a->rhs.get());
        auto hit = cse.find(key);
        if (hit != cse.end() && hit->second.var != a->name) {
            a->rhs = std::make_unique<VarNode>(hit->second.var);
            changed = true;
        }
        // x = x 为空操作。
        if (auto* rv = dynamic_cast<VarNode*>(a->rhs.get())) {
            if (rv->name == a->name) { s = nullptr; changed = true; return; }
        }
        if (envIsLocal(a->name)) {
            envKillCopiesOf(a->name);
            cseKillVar(a->name);
            Val av = valOfExpr(a->rhs.get());
            if (av.kind == 2 && av.src == a->name) av = Val{};   // 防自复写
            envSet(a->name, av);
            if (auto* bin = dynamic_cast<BinaryNode*>(a->rhs.get())) {
                (void)bin;
                if (!exprHasCall(a->rhs.get())) {
                    std::unordered_set<std::string> deps;
                    collectVarNames(a->rhs.get(), deps);
                    bool ok = !deps.empty() && !deps.count(a->name);
                    for (const auto& n : deps) if (!envIsLocal(n)) { ok = false; break; }
                    if (ok) cse[exprKey(a->rhs.get())] = {a->name, deps};
                }
            }
        }
        return;
    }

    if (auto* es = dynamic_cast<ExprStmtNode*>(s.get())) {
        if (es->expr == nullptr || !exprHasCall(es->expr.get())) {
            s = nullptr;   // 纯表达式语句无副作用
            changed = true;
            return;
        }
        foldExpr(es->expr);
        return;
    }

    if (auto* r = dynamic_cast<ReturnNode*>(s.get())) {
        if (r->retExpr != nullptr) foldExpr(r->retExpr);
        return;
    }

    if (auto* i = dynamic_cast<IfNode*>(s.get())) {
        foldExpr(i->condition);
        int cv;
        if (isLit(i->condition.get(), cv)) {
            // 条件为常量：直接以选中的分支替换整个 if。
            std::unique_ptr<StmtNode> taken =
                (cv != 0) ? std::move(i->thenStmt) : std::move(i->elseStmt);
            if (taken == nullptr) taken = std::make_unique<BlockNode>();
            s = std::move(taken);
            changed = true;
            foldStmt(s);
            return;
        }
        cse.clear();
        Env base = env;
        foldStmt(i->thenStmt);
        Env afterThen = std::move(env);
        env = std::move(base);
        if (i->elseStmt != nullptr) foldStmt(i->elseStmt);
        envIntersect(env, afterThen);
        cse.clear();
        return;
    }

    if (auto* w = dynamic_cast<WhileNode*>(s.get())) {
        // 循环体内被赋值/声明的名字在条件与体内不可信。
        std::unordered_set<std::string> killed;
        collectAssignedNames(w->body.get(), killed);
        for (const auto& n : killed) {
            envSet(n, Val{});
            envKillCopiesOf(n);
        }
        cse.clear();
        foldExpr(w->condition);
        int cv;
        if (isLit(w->condition.get(), cv) && cv == 0) {
            s = nullptr;   // 循环从不执行
            changed = true;
            return;
        }
        foldStmt(w->body);
        // 循环之后：体内建立的事实不可保留，回到 kill 后的状态。
        for (const auto& n : killed) {
            envSet(n, Val{});
            envKillCopiesOf(n);
        }
        cse.clear();
        return;
    }
    // Break / Continue：无事实变化。
}

void Optimizer::foldExpr(std::unique_ptr<ExprNode>& e) {
    if (e == nullptr) return;

    if (auto* var = dynamic_cast<VarNode*>(e.get())) {
        if (envIsLocal(var->name)) {
            Val* v = envLookup(var->name);
            if (v != nullptr && v->kind == 1) {
                e = std::make_unique<LiteralNode>(v->c);
                changed = true;
                return;
            }
            if (v != nullptr && v->kind == 2 && v->src != var->name) {
                std::string src = v->src;
                Val* sv = envLookup(src);
                if (sv != nullptr && sv->kind == 1) {
                    e = std::make_unique<LiteralNode>(sv->c);
                } else {
                    e = std::make_unique<VarNode>(src);
                }
                changed = true;
                return;
            }
        } else {
            auto it = constGlobals.find(var->name);
            if (it != constGlobals.end()) {
                e = std::make_unique<LiteralNode>(it->second);
                changed = true;
            }
        }
        return;
    }

    if (auto* un = dynamic_cast<UnaryNode*>(e.get())) {
        foldExpr(un->operand);
        int v;
        if (un->op == "+") { e = std::move(un->operand); changed = true; return; }
        if (isLit(un->operand.get(), v)) {
            int out = (un->op == "-") ? wrap32(-(long long)v) : (v == 0 ? 1 : 0);
            e = std::make_unique<LiteralNode>(out);
            changed = true;
            return;
        }
        if (un->op == "-") {
            if (auto* inner = dynamic_cast<UnaryNode*>(un->operand.get())) {
                if (inner->op == "-") {   // -(-x) → x
                    e = std::move(inner->operand);
                    changed = true;
                    return;
                }
            }
        }
        return;
    }

    if (auto* call = dynamic_cast<CallNode*>(e.get())) {
        for (auto& a : call->args) foldExpr(a);
        return;
    }

    auto* bin = dynamic_cast<BinaryNode*>(e.get());
    if (bin == nullptr) return;
    foldExpr(bin->left);
    foldExpr(bin->right);
    const std::string& op = bin->op;
    int lv, rv;
    bool lLit = isLit(bin->left.get(), lv);
    bool rLit = isLit(bin->right.get(), rv);

    // 短路语义允许：0&&e / 1||e 直接定值并丢弃 e（e 本就不会执行）。
    if (op == "&&" && lLit && lv == 0) { e = std::make_unique<LiteralNode>(0); changed = true; return; }
    if (op == "||" && lLit && lv != 0) { e = std::make_unique<LiteralNode>(1); changed = true; return; }

    if (lLit && rLit) {
        long long L = lv, R = rv;
        int out;
        bool ok = true;
        if (op == "+") out = wrap32(L + R);
        else if (op == "-") out = wrap32(L - R);
        else if (op == "*") out = wrap32(L * R);
        else if (op == "/") { if (rv == 0) ok = false; else out = wrap32(L / R); }
        else if (op == "%") { if (rv == 0) ok = false; else out = wrap32(L % R); }
        else if (op == "<") out = lv < rv;
        else if (op == ">") out = lv > rv;
        else if (op == "<=") out = lv <= rv;
        else if (op == ">=") out = lv >= rv;
        else if (op == "==") out = lv == rv;
        else if (op == "!=") out = lv != rv;
        else if (op == "&&") out = (lv != 0 && rv != 0) ? 1 : 0;
        else if (op == "||") out = (lv != 0 || rv != 0) ? 1 : 0;
        else ok = false;
        if (ok) { e = std::make_unique<LiteralNode>(out); changed = true; return; }
    }

    bool leftPure = !exprHasCall(bin->left.get());
    bool rightPure = !exprHasCall(bin->right.get());

    // 1&&x / 0||x / x&&1 / x||0 → x 的布尔化；比较与 ! 已是 0/1 可直接取用。
    auto boolize = [&](std::unique_ptr<ExprNode> x) -> std::unique_ptr<ExprNode> {
        if (auto* b2 = dynamic_cast<BinaryNode*>(x.get())) {
            const std::string& o = b2->op;
            if (o == "<" || o == ">" || o == "<=" || o == ">=" || o == "==" || o == "!=" ||
                o == "&&" || o == "||") {
                return x;
            }
        }
        if (auto* u2 = dynamic_cast<UnaryNode*>(x.get())) {
            if (u2->op == "!") return x;
        }
        auto inner = std::make_unique<UnaryNode>("!", std::move(x));
        return std::make_unique<UnaryNode>("!", std::move(inner));
    };
    if (op == "&&" && lLit && lv != 0) { e = boolize(std::move(bin->right)); changed = true; return; }
    if (op == "||" && lLit && lv == 0) { e = boolize(std::move(bin->right)); changed = true; return; }
    if (op == "&&" && rLit && rv != 0) { e = boolize(std::move(bin->left)); changed = true; return; }
    if (op == "||" && rLit && rv == 0) { e = boolize(std::move(bin->left)); changed = true; return; }
    if (op == "&&" && rLit && rv == 0 && leftPure) { e = std::make_unique<LiteralNode>(0); changed = true; return; }
    if (op == "||" && rLit && rv != 0 && leftPure) { e = std::make_unique<LiteralNode>(1); changed = true; return; }

    // 结构相同的纯操作数：x-x=0、x==x=1、x<x=0 等。
    if (leftPure && rightPure && exprKey(bin->left.get()) == exprKey(bin->right.get())) {
        if (op == "-") { e = std::make_unique<LiteralNode>(0); changed = true; return; }
        if (op == "==" || op == "<=" || op == ">=") { e = std::make_unique<LiteralNode>(1); changed = true; return; }
        if (op == "!=" || op == "<" || op == ">") { e = std::make_unique<LiteralNode>(0); changed = true; return; }
    }

    // 规范化：常量放右侧（+、*）；x - c → x + (-c)。
    if ((op == "+" || op == "*") && lLit && !rLit) {
        std::swap(bin->left, bin->right);
        std::swap(lLit, rLit);
        std::swap(lv, rv);
        changed = true;
    }
    if (op == "-" && rLit) {
        bin->op = "+";
        bin->right = std::make_unique<LiteralNode>(wrap32(-(long long)rv));
        foldExpr(e);   // 交给 + 的规则继续化简
        return;
    }

    // 代数恒等式。
    if (op == "+" && rLit && rv == 0) { e = std::move(bin->left); changed = true; return; }
    if (op == "*" && rLit && rv == 1) { e = std::move(bin->left); changed = true; return; }
    if (op == "*" && rLit && rv == 0 && leftPure) { e = std::make_unique<LiteralNode>(0); changed = true; return; }
    if (op == "/" && rLit && rv == 1) { e = std::move(bin->left); changed = true; return; }
    if (op == "%" && rLit && rv == 1 && leftPure) { e = std::make_unique<LiteralNode>(0); changed = true; return; }

    // 重结合：(x + c1) + c2 → x + (c1+c2)；(x * c1) * c2 → x * (c1*c2)。
    if ((op == "+" || op == "*") && rLit) {
        if (auto* lb = dynamic_cast<BinaryNode*>(bin->left.get())) {
            int ic;
            if (lb->op == op && isLit(lb->right.get(), ic)) {
                long long combined = (op == "+") ? (long long)ic + rv : (long long)ic * rv;
                bin->left = std::move(lb->left);
                bin->right = std::make_unique<LiteralNode>(wrap32(combined));
                changed = true;
                foldExpr(e);
                return;
            }
        }
    }
}

// ---------- 死代码删除 ----------

void Optimizer::resolveAssigns(FuncDefNode* f) {
    globalAssigns.clear();
    std::vector<std::unordered_set<std::string>> scopes;
    scopes.push_back({});
    for (const auto& p : f->params) scopes.back().insert(p);
    resolveAssignsStmt(f->body.get(), scopes);
}

void Optimizer::resolveAssignsStmt(StmtNode* s,
                                   std::vector<std::unordered_set<std::string>>& scopes) {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        scopes.push_back({});
        for (const auto& c : block->stmts) resolveAssignsStmt(c.get(), scopes);
        scopes.pop_back();
        return;
    }
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) { scopes.back().insert(d->name); return; }
    if (auto* a = dynamic_cast<AssignNode*>(s)) {
        bool local = false;
        for (const auto& sc : scopes) if (sc.count(a->name)) { local = true; break; }
        if (!local) globalAssigns.insert(a);
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        resolveAssignsStmt(i->thenStmt.get(), scopes);
        resolveAssignsStmt(i->elseStmt.get(), scopes);
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        resolveAssignsStmt(w->body.get(), scopes);
        return;
    }
}

bool Optimizer::stmtEssential(StmtNode* s, const std::unordered_set<std::string>& live) const {
    if (s == nullptr) return false;
    if (dynamic_cast<ReturnNode*>(s) || dynamic_cast<BreakNode*>(s) ||
        dynamic_cast<ContinueNode*>(s)) {
        return true;
    }
    if (auto* es = dynamic_cast<ExprStmtNode*>(s)) return exprHasCall(es->expr.get());
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) {
        return exprHasCall(d->initExpr.get()) || live.count(d->name) > 0;
    }
    if (auto* a = dynamic_cast<AssignNode*>(s)) {
        return globalAssigns.count(a) > 0 || exprHasCall(a->rhs.get()) || live.count(a->name) > 0;
    }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        return exprHasCall(i->condition.get()) || stmtEssential(i->thenStmt.get(), live) ||
               stmtEssential(i->elseStmt.get(), live);
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        // 常量真条件的循环（如 while(1)）可能是有意的无限循环，不可删除。
        int cv;
        if (isLit(w->condition.get(), cv) && cv != 0) return true;
        return exprHasCall(w->condition.get()) || stmtEssential(w->body.get(), live);
    }
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        for (const auto& c : block->stmts) if (stmtEssential(c.get(), live)) return true;
        return false;
    }
    return true;
}

void Optimizer::markReads(StmtNode* s, const std::unordered_set<std::string>& live,
                          std::unordered_set<std::string>& out) const {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        for (const auto& c : block->stmts) markReads(c.get(), live, out);
        return;
    }
    if (auto* r = dynamic_cast<ReturnNode*>(s)) { collectVarNames(r->retExpr.get(), out); return; }
    if (auto* es = dynamic_cast<ExprStmtNode*>(s)) {
        if (exprHasCall(es->expr.get())) collectVarNames(es->expr.get(), out);
        return;
    }
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) {
        if (stmtEssential(s, live)) collectVarNames(d->initExpr.get(), out);
        return;
    }
    if (auto* a = dynamic_cast<AssignNode*>(s)) {
        if (stmtEssential(s, live)) {
            collectVarNames(a->rhs.get(), out);
            out.insert(a->name);   // 存储位置需保留（声明不可删）
        }
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        if (stmtEssential(s, live)) {
            collectVarNames(i->condition.get(), out);
            markReads(i->thenStmt.get(), live, out);
            markReads(i->elseStmt.get(), live, out);
        }
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        if (stmtEssential(s, live)) {
            collectVarNames(w->condition.get(), out);
            markReads(w->body.get(), live, out);
        }
        return;
    }
}

void Optimizer::pruneStmts(StmtNode* s, const std::unordered_set<std::string>& live) {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        auto& v = block->stmts;
        size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const std::unique_ptr<StmtNode>& c) {
                                   return !stmtEssential(c.get(), live);
                               }),
                v.end());
        if (v.size() != before) changed = true;
        for (auto& c : v) pruneStmts(c.get(), live);
        return;
    }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        pruneStmts(i->thenStmt.get(), live);
        pruneStmts(i->elseStmt.get(), live);
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        pruneStmts(w->body.get(), live);
        return;
    }
}

bool Optimizer::dceFunc(FuncDefNode* f) {
    resolveAssigns(f);
    std::unordered_set<std::string> live;
    for (int iter = 0; iter < 32; ++iter) {
        std::unordered_set<std::string> next = live;
        markReads(f->body.get(), live, next);
        if (next.size() == live.size()) break;
        live = std::move(next);
    }
    pruneStmts(f->body.get(), live);
    return true;
}

// ---------- 循环不变量外提 ----------

bool Optimizer::invariantOk(ExprNode* e, const std::unordered_set<std::string>& killed,
                            bool bodyHasCall) const {
    if (e == nullptr) return false;
    int v;
    if (isLit(e, v)) return true;
    if (auto* var = dynamic_cast<VarNode*>(e)) {
        if (killed.count(var->name)) return false;
        // 循环体内含调用时，全局变量可能被被调函数修改，不能视为不变量。
        if (bodyHasCall && allGlobals.count(var->name)) return false;
        return true;
    }
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        return invariantOk(un->operand.get(), killed, bodyHasCall);
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        return invariantOk(bin->left.get(), killed, bodyHasCall) &&
               invariantOk(bin->right.get(), killed, bodyHasCall);
    }
    return false;   // 调用不纯
}

namespace {
void countOpsVars(ExprNode* e, int& ops, int& vars, bool& hasMul) {
    if (e == nullptr) return;
    if (dynamic_cast<VarNode*>(e)) { vars++; return; }
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        ops++;
        countOpsVars(un->operand.get(), ops, vars, hasMul);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        ops++;
        if (bin->op == "*" || bin->op == "/" || bin->op == "%") hasMul = true;
        countOpsVars(bin->left.get(), ops, vars, hasMul);
        countOpsVars(bin->right.get(), ops, vars, hasMul);
    }
}
}  // namespace

void Optimizer::collectInvariants(ExprNode* e, const std::unordered_set<std::string>& killed,
                                  bool bodyHasCall,
                                  std::vector<ExprNode*>& out,
                                  std::unordered_set<std::string>& seenKeys) const {
    if (e == nullptr) return;
    if (dynamic_cast<BinaryNode*>(e) != nullptr && invariantOk(e, killed, bodyHasCall)) {
        int ops = 0, vars = 0;
        bool hasMul = false;
        countOpsVars(e, ops, vars, hasMul);
        // 值得外提：含变量，且要么有乘/除/模，要么至少两个运算。
        if (vars >= 1 && (hasMul || ops >= 2)) {
            std::string key = exprKey(e);
            if (!seenKeys.count(key)) {
                seenKeys.insert(key);
                out.push_back(e);
            }
            return;   // 取最大不变子树，不再深入
        }
    }
    if (auto* un = dynamic_cast<UnaryNode*>(e)) {
        collectInvariants(un->operand.get(), killed, bodyHasCall, out, seenKeys);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e)) {
        collectInvariants(bin->left.get(), killed, bodyHasCall, out, seenKeys);
        collectInvariants(bin->right.get(), killed, bodyHasCall, out, seenKeys);
        return;
    }
    if (auto* call = dynamic_cast<CallNode*>(e)) {
        for (const auto& a : call->args) {
            collectInvariants(a.get(), killed, bodyHasCall, out, seenKeys);
        }
    }
}

void Optimizer::replaceSubtrees(std::unique_ptr<ExprNode>& e, const std::string& key,
                                const std::string& varName) {
    if (e == nullptr) return;
    if (exprKey(e.get()) == key) {
        e = std::make_unique<VarNode>(varName);
        return;
    }
    if (auto* un = dynamic_cast<UnaryNode*>(e.get())) {
        replaceSubtrees(un->operand, key, varName);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryNode*>(e.get())) {
        replaceSubtrees(bin->left, key, varName);
        replaceSubtrees(bin->right, key, varName);
        return;
    }
    if (auto* call = dynamic_cast<CallNode*>(e.get())) {
        for (auto& a : call->args) replaceSubtrees(a, key, varName);
    }
}

void Optimizer::replaceInStmt(StmtNode* s, const std::string& key, const std::string& varName) {
    if (s == nullptr) return;
    if (auto* block = dynamic_cast<BlockNode*>(s)) {
        for (auto& c : block->stmts) replaceInStmt(c.get(), key, varName);
        return;
    }
    if (auto* d = dynamic_cast<DeclStmtNode*>(s)) { replaceSubtrees(d->initExpr, key, varName); return; }
    if (auto* a = dynamic_cast<AssignNode*>(s)) { replaceSubtrees(a->rhs, key, varName); return; }
    if (auto* i = dynamic_cast<IfNode*>(s)) {
        replaceSubtrees(i->condition, key, varName);
        replaceInStmt(i->thenStmt.get(), key, varName);
        replaceInStmt(i->elseStmt.get(), key, varName);
        return;
    }
    if (auto* w = dynamic_cast<WhileNode*>(s)) {
        replaceSubtrees(w->condition, key, varName);
        replaceInStmt(w->body.get(), key, varName);
        return;
    }
    if (auto* r = dynamic_cast<ReturnNode*>(s)) { replaceSubtrees(r->retExpr, key, varName); return; }
    if (auto* es = dynamic_cast<ExprStmtNode*>(s)) { replaceSubtrees(es->expr, key, varName); return; }
}

void Optimizer::hoistLoop(BlockNode* parent, size_t pos) {
    auto* w = dynamic_cast<WhileNode*>(parent->stmts[pos].get());
    if (w == nullptr) return;
    std::unordered_set<std::string> killed;
    collectAssignedNames(w->body.get(), killed);
    bool bodyHasCall = stmtHasCall(w->body.get()) || exprHasCall(w->condition.get());

    std::vector<ExprNode*> cands;
    std::unordered_set<std::string> seen;
    collectInvariants(w->condition.get(), killed, bodyHasCall, cands, seen);
    // 遍历体内全部表达式（含嵌套结构）收集不变子树。
    struct Walker {
        Optimizer* opt;
        const std::unordered_set<std::string>& killed;
        bool bodyHasCall;
        std::vector<ExprNode*>& cands;
        std::unordered_set<std::string>& seen;
        void stmt(StmtNode* s) {
            if (s == nullptr) return;
            if (auto* b = dynamic_cast<BlockNode*>(s)) { for (auto& c : b->stmts) stmt(c.get()); return; }
            if (auto* d = dynamic_cast<DeclStmtNode*>(s)) { opt->collectInvariants(d->initExpr.get(), killed, bodyHasCall, cands, seen); return; }
            if (auto* a = dynamic_cast<AssignNode*>(s)) { opt->collectInvariants(a->rhs.get(), killed, bodyHasCall, cands, seen); return; }
            if (auto* i = dynamic_cast<IfNode*>(s)) {
                opt->collectInvariants(i->condition.get(), killed, bodyHasCall, cands, seen);
                stmt(i->thenStmt.get());
                stmt(i->elseStmt.get());
                return;
            }
            if (auto* w2 = dynamic_cast<WhileNode*>(s)) {
                opt->collectInvariants(w2->condition.get(), killed, bodyHasCall, cands, seen);
                stmt(w2->body.get());
                return;
            }
            if (auto* r = dynamic_cast<ReturnNode*>(s)) { opt->collectInvariants(r->retExpr.get(), killed, bodyHasCall, cands, seen); return; }
            if (auto* e = dynamic_cast<ExprStmtNode*>(s)) { opt->collectInvariants(e->expr.get(), killed, bodyHasCall, cands, seen); return; }
        }
    } walker{this, killed, bodyHasCall, cands, seen};
    walker.stmt(w->body.get());

    // 限量外提，避免寄存器压力失衡。
    size_t limit = 4;
    size_t count = std::min(cands.size(), limit);
    for (size_t k = 0; k < count; ++k) {
        std::string key = exprKey(cands[k]);
        std::string name = "@licm" + std::to_string(++licmCounter);
        auto init = cloneExpr(cands[k]);
        replaceSubtrees(w->condition, key, name);
        replaceInStmt(w->body.get(), key, name);
        parent->stmts.insert(parent->stmts.begin() + pos,
                             std::make_unique<DeclStmtNode>(false, name, std::move(init)));
        pos++;
        changed = true;
    }
}

bool Optimizer::licmBlock(BlockNode* block, bool /*unused*/) {
    for (size_t i = 0; i < block->stmts.size(); ++i) {
        StmtNode* s = block->stmts[i].get();
        if (auto* w = dynamic_cast<WhileNode*>(s)) {
            if (auto* b = dynamic_cast<BlockNode*>(w->body.get())) licmBlock(b, false);
            size_t sizeBefore = block->stmts.size();
            hoistLoop(block, i);
            i += block->stmts.size() - sizeBefore;   // 跳过新插入的声明
            continue;
        }
        if (auto* ifn = dynamic_cast<IfNode*>(s)) {
            if (auto* b = dynamic_cast<BlockNode*>(ifn->thenStmt.get())) licmBlock(b, false);
            if (auto* b = dynamic_cast<BlockNode*>(ifn->elseStmt.get())) licmBlock(b, false);
            continue;
        }
        if (auto* b = dynamic_cast<BlockNode*>(s)) licmBlock(b, false);
    }
    return true;
}

bool Optimizer::licmFunc(FuncDefNode* f) {
    return licmBlock(f->body.get(), false);
}

// ---------- 顶层 ----------

void Optimizer::run(CompUnitNode* root) {
    allGlobals.clear();
    constGlobals.clear();
    for (const auto& item : root->items) {
        if (auto* d = dynamic_cast<DeclStmtNode*>(item.get())) allGlobals.insert(d->name);
    }
    for (const auto& item : root->items) {
        if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) {
            std::unique_ptr<StmtNode> body(f->body.release());
            normalizeStmt(body);
            f->body.reset(static_cast<BlockNode*>(body.release()));
        }
    }
    computeConstGlobals(root);

    for (int iter = 0; iter < 4; ++iter) {
        changed = false;
        for (const auto& item : root->items) {
            if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) foldFunc(f);
        }
        for (const auto& item : root->items) {
            if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) dceFunc(f);
        }
        for (const auto& item : root->items) {
            if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) licmFunc(f);
        }
        if (!changed) break;
    }
    // 收尾折叠：清理 LICM 引入的复写链。
    for (const auto& item : root->items) {
        if (auto* f = dynamic_cast<FuncDefNode*>(item.get())) foldFunc(f);
    }
}
