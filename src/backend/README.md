# 后端 (Backend) —— 负责人 D

后端负责把中端产出的、经过语义检查的 AST（带符号信息）翻译成 **RISC-V32 汇编**，
从 stdin 读源程序、向 stdout 写汇编，并在此基础上做优化与整机联调。
设计细节见 [`docs/backend-design.md`](../../docs/backend-design.md)，语言规格见 [`docs/toyc-spec.md`](../../docs/toyc-spec.md)。

## 子模块

| 目录 | 模块 | 说明 |
| --- | --- | --- |
| `codegen/` | 代码生成 | 遍历 AST 生成 RV32 指令；含调用约定、栈帧、全局/常量、短路跳转、寄存器分配与优化 |
| `regalloc/` | 寄存器分配 | 局部/形参 → `s1–s11`、表达式临时值 → `t0–t5`（已内建于 `codegen`） |
| `opt/` | 优化 | 常量折叠/传播、强度削弱、窥孔、跳转短路（`-opt` 打开，已内建于 `codegen`） |

## 与其它模块的接口约定（联调）

后端的输入来自中端 (C)，具体契约见 `docs/backend-design.md` §1：

- **输入**：经中端 (C) 语义检查的 AST（`CheckedAST.root`）。名字解析、作用域/遮蔽、常量求值
  与存储分配（全局标签 / `s` 寄存器 / 栈槽）均由后端在代码生成时完成。
- **输出**：向 **stdout** 写 RV32 汇编，可被 `riscv64-*-gcc -march=rv32im -mabi=ilp32` 汇编链接。
- 头文件接口放在 `include/backend/`，实现放在本目录。

## 实现顺序（均已完成）

1. 打通「最小可运行」：`int main(){ return N; }` → RV32，能在 qemu-riscv32 上返回正确退出码。
2. 表达式与算术/关系/一元运算、局部变量与栈帧。
3. 控制流：`if / while`、短路求值、逻辑运算、`break`/`continue`。
4. 函数定义与调用（RISC-V 调用约定：a0–a7 传参、ra/sp 维护，支持递归）。
5. 全局变量与常量。
6. 优化 pass（`opt/`）。
7. 与前端/中端整机联调 + 回归测试。

## 目标平台约定

- 指令集：**RV32IM**（乘除模用 M 扩展），ABI = **ilp32**
- 调用约定：标准 RISC-V ABI（`sp` 16 字节对齐，`a0` 返回值/传参，`ra` 返回地址）
- 验证：`riscv64-linux-gnu-gcc -march=rv32im -mabi=ilp32 -static` 汇编链接 +
  `qemu-riscv32` 运行，比对 `main` 返回值（退出码）

## 状态

已实现并联调通过。`codegen/CodeGen.cpp` 完成 AST → RV32IM 直接翻译（栈帧、表达式、
控制流与跳转短路、函数调用与递归、全局/常量），并含 `-opt` 优化（常量折叠/传播、
乘 2 的幂强度削弱、窥孔清理）。接口见 `include/backend/CodeGen.h`，联调修改记录见
[`docs/integration-notes.md`](../../docs/integration-notes.md)。功能用例 19/19 通过。

> 寄存器分配与优化均内建在 `codegen/`；`regalloc/`、`opt/` 为空目录，留作进一步拆分/扩展。
