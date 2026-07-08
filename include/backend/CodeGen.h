#pragma once

#include "AST.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <ostream>

// RISC-V32 (RV32IM / ilp32) 代码生成器。
// 输入：语义检查通过的前端 AST（CompUnitNode）。
// 输出：向给定流写出可被 riscv64-*-gcc -march=rv32im -mabi=ilp32 汇编链接的汇编。
//
// 求值模型：每个表达式的结果落在 a0；二元运算的左操作数临时溢出到函数栈帧内
// 固定的临时槽（sp 在函数体内保持不变，因此调用点始终 16 字节对齐）。
class CodeGen {
public:
    explicit CodeGen(bool optimize = false);
    void generate(CompUnitNode* root, std::ostream& out);

private:
    // 变量引用解析到的三种存储形态。
    struct VarInfo {
        enum Kind { Const, Global, Local } kind;
        int constVal;       // Const
        std::string label;  // Global
        int offset;         // Local：相对 s0 的（负）偏移
    };

    bool opt;
    int labelCounter;

    // 作用域栈：scopes[0] 为全局作用域。
    std::vector<std::unordered_map<std::string, VarInfo>> scopes;

    // 当前函数状态。
    std::vector<std::string> cur;              // 当前函数的指令行
    std::vector<std::string> textLines;        // 所有函数汇编
    std::vector<std::string> dataLines;        // .data 段
    int totalLocals;
    int localCursor;                           // 已分配的局部槽数
    std::string epilogueLabel;
    std::vector<std::pair<std::string, std::string>> loopLabels; // (continue, break)

    // 作用域与符号。
    void pushScope();
    void popScope();
    const VarInfo* lookupVar(const std::string& name) const;

    // 发射辅助。
    void emit(const std::string& line);
    void emitLabel(const std::string& label);
    std::string newLabel();
    void memInsn(const std::string& op, const std::string& reg, int offset); // 以 s0 为基址
    void loadImm(const std::string& reg, int value);

    int localOffset(int slot) const;  // 局部槽 slot 的 s0 偏移
    int tempOffset(int slot) const;   // 临时槽 slot 的 s0 偏移
    void storeTemp(int slot);         // sw a0, tempOffset
    void loadTemp(int slot, const std::string& reg);

    // 帧尺寸预计算。
    int countLocalDecls(StmtNode* node) const;
    int exprTempNeed(ExprNode* node) const;
    int stmtTempNeed(StmtNode* node) const;

    // 编译期常量求值（用于常量折叠与全局初值）。
    bool tryEvalConst(ExprNode* node, int& out) const;

    // 代码生成。
    void genFunc(FuncDefNode* func);
    void genStmt(StmtNode* node);
    void genBlock(BlockNode* node, bool createScope);
    void genExpr(ExprNode* node, int depth);
    void genLogical(BinaryNode* node, int depth);
    void genCond(ExprNode* node, const std::string& labelTrue,
                 const std::string& labelFalse, int depth);
    void genCall(CallNode* node, int depth);

    void peephole();
};
