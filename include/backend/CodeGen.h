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
// 存储模型：
//   - 局部变量/形参优先分配到被调用者保存寄存器 s1..s11（跨调用安全），超出部分落栈。
//   - 表达式结果落在 a0；二元运算的左操作数在“右子树不含函数调用”且深度够浅时暂存于
//     调用者保存寄存器 t0..t5，否则溢出到栈帧内的临时槽（保证跨调用安全）。
//   - t6 用作大偏移寻址/内存临时值的搬运暂存，不参与上面的分配。
//   函数体内 sp 保持不变（仅传递 >8 个实参时临时调整），故调用点始终 16 字节对齐。
class CodeGen {
public:
    explicit CodeGen(bool optimize = false);
    void generate(CompUnitNode* root, std::ostream& out);

private:
    struct VarInfo {
        enum Kind { Const, Global, Local, Reg } kind;
        int constVal;       // Const
        std::string label;  // Global
        int offset;         // Local：相对 s0 的（负）偏移
        std::string reg;    // Reg：s1..s11
    };

    bool opt;
    int labelCounter;

    std::vector<std::unordered_map<std::string, VarInfo>> scopes;

    // 当前函数状态。
    std::vector<std::string> cur;
    std::vector<std::string> textLines;
    std::vector<std::string> dataLines;
    int totalLocals;
    int regLocals;          // 落入 s 寄存器的局部数量 = min(totalLocals, 11)
    int stackLocalBase;     // 栈上局部区起始偏移（相对 s0）
    int tempBaseOffset;     // 临时区起始偏移（相对 s0）
    int localCursor;
    bool funcHasCall;       // 叶子函数可省去 ra 的保存/恢复
    bool aRegLeaf;          // 叶子函数：局部全部放入调用者保存寄存器 a1..a7，完全省去栈帧
    std::string epilogueLabel;
    std::vector<std::pair<std::string, std::string>> loopLabels;

    void pushScope();
    void popScope();
    const VarInfo* lookupVar(const std::string& name) const;
    VarInfo assignLocalLocation();
    bool tryGetReg(ExprNode* node, std::string& reg) const;

    void emit(const std::string& line);
    void emitLabel(const std::string& label);
    std::string newLabel();
    void memInsn(const std::string& op, const std::string& reg, int offset);
    void loadImm(const std::string& reg, int value);

    int tempOffset(int slot) const;
    void storeTemp(int slot);
    void loadTemp(int slot, const std::string& reg);

    int countLocalDecls(StmtNode* node) const;
    int exprTempNeed(ExprNode* node) const;
    int stmtTempNeed(StmtNode* node) const;
    bool containsCall(ExprNode* node) const;
    bool stmtContainsCall(StmtNode* node) const;
    // 表达式求值可能达到的最大嵌套深度（上界，用于判断叶子函数是否需要栈上临时槽）。
    int exprMaxDepth(ExprNode* node, int depth) const;
    int stmtMaxDepth(StmtNode* node) const;

    bool tryEvalConst(ExprNode* node, int& out) const;

    void genFunc(FuncDefNode* func);
    void genStmt(StmtNode* node);
    void genBlock(BlockNode* node, bool createScope);
    void genExpr(ExprNode* node, int depth);
    void genLogical(BinaryNode* node, int depth);
    void genCondJump(ExprNode* node, const std::string& target,
                     bool jumpWhenTrue, int depth);
    void genCall(CallNode* node, int depth);
    bool tryEmitRegBinary(BinaryNode* node);
    bool tryEmitOptimizedAssign(AssignNode* node);
    // 计算二元运算左操作数：结果寄存器名经 leftReg 返回，右操作数已在 a0。
    void genBinaryOperands(BinaryNode* node, int depth, std::string& leftReg);

    void peephole();
};
