#pragma once

#include "AST.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// AST 级优化器（仅 -opt 时运行，位于语义检查之后、代码生成之前）。
// ToyC 无指针/数组，别名分析平凡，因此以下经典优化可以安全实现：
//   1. 全局常量传播：全程未被赋值的全局变量以其常量初值内联；
//   2. 局部常量/复写传播 + 常量折叠 + 代数化简（带作用域的顺序数据流）；
//   3. 公共子表达式消除（线性区间内，声明/赋值右值级）；
//   4. 死代码删除（mark-sweep：无副作用且结果不被读取的语句/整个循环）；
//   5. 循环不变量外提（纯表达式提升为循环前的编译器临时量）。
// 各 pass 迭代运行至不动点（有限轮数封顶）。
class Optimizer {
public:
    void run(CompUnitNode* root);

private:
    // ---------- 全局信息 ----------
    std::unordered_set<std::string> allGlobals;            // 全部全局变量名
    std::unordered_map<std::string, int> constGlobals;     // 全程未被赋值的全局 → 常量值
    int licmCounter = 0;
    bool changed = false;

    // ---------- 通用工具 ----------
    static int wrap32(long long v);
    static bool isLit(ExprNode* e, int& v);
    static bool exprHasCall(ExprNode* e);
    static bool stmtHasCall(StmtNode* s);
    static void collectVarNames(ExprNode* e, std::unordered_set<std::string>& out);
    static void collectAssignedNames(StmtNode* s, std::unordered_set<std::string>& out);
    static std::string exprKey(ExprNode* e);
    static std::unique_ptr<ExprNode> cloneExpr(ExprNode* e);

    void computeConstGlobals(CompUnitNode* root);
    // 把 if/while 的非块状分支/循环体包一层 Block，统一后续处理。
    void normalizeStmt(std::unique_ptr<StmtNode>& s);

    // ---------- 常量/复写传播 + 折叠 + 代数化简 + CSE ----------
    struct Val {
        int kind = 0;        // 0 未知, 1 常量, 2 某局部变量的复写
        int c = 0;
        std::string src;
    };
    using Env = std::vector<std::unordered_map<std::string, Val>>;
    Env env;
    struct CseEntry {
        std::string var;
        std::unordered_set<std::string> deps;   // 表达式引用的变量
    };
    std::unordered_map<std::string, CseEntry> cse;   // 表达式键 → 持有该值的变量

    bool envIsLocal(const std::string& name) const;
    Val* envLookup(const std::string& name);
    void envSet(const std::string& name, const Val& v);
    void envKillCopiesOf(const std::string& name);
    void cseKillVar(const std::string& name);
    static void envIntersect(Env& a, const Env& b);

    void foldFunc(FuncDefNode* f);
    void foldStmt(std::unique_ptr<StmtNode>& s);
    void foldExpr(std::unique_ptr<ExprNode>& e);
    Val valOfExpr(ExprNode* e);

    // ---------- 死代码删除 ----------
    // assignIsGlobal：解析每个赋值语句的目标是否全局（考虑遮蔽，逐作用域解析）。
    std::unordered_set<const AssignNode*> globalAssigns;
    void resolveAssigns(FuncDefNode* f);
    void resolveAssignsStmt(StmtNode* s, std::vector<std::unordered_set<std::string>>& scopes);
    bool dceFunc(FuncDefNode* f);
    bool stmtEssential(StmtNode* s, const std::unordered_set<std::string>& live) const;
    void markReads(StmtNode* s, const std::unordered_set<std::string>& live,
                   std::unordered_set<std::string>& out) const;
    void pruneStmts(StmtNode* s, const std::unordered_set<std::string>& live);

    // ---------- 循环不变量外提 ----------
    bool licmFunc(FuncDefNode* f);
    bool licmBlock(BlockNode* block, bool bodyHasCallOuter);
    void hoistLoop(BlockNode* parent, size_t pos);
    void collectInvariants(ExprNode* e, const std::unordered_set<std::string>& killed,
                           bool bodyHasCall,
                           std::vector<ExprNode*>& out,
                           std::unordered_set<std::string>& seenKeys) const;
    bool invariantOk(ExprNode* e, const std::unordered_set<std::string>& killed,
                     bool bodyHasCall) const;
    void collectExprsOfStmt(StmtNode* s, std::vector<ExprNode**>& out);
    static void replaceSubtrees(std::unique_ptr<ExprNode>& e, const std::string& key,
                                const std::string& varName);
    void replaceInStmt(StmtNode* s, const std::string& key, const std::string& varName);
};
