# 后端 (Backend) —— 负责人 D

后端负责把中端产出的、经过语义检查的 AST（带符号信息）翻译成 **RISC-V32 汇编**，
从 stdin 读源程序、向 stdout 写汇编，并在此基础上做优化与整机联调。
设计细节见 [`docs/backend-design.md`](../../docs/backend-design.md)，语言规格见 [`docs/toyc-spec.md`](../../docs/toyc-spec.md)。

## 子模块

| 目录 | 模块 | 说明 |
| --- | --- | --- |
| `codegen/` | 代码生成 | 遍历 AST，生成 RV32 指令序列；处理函数调用约定、栈帧、全局变量、短路求值 |
| `regalloc/` | 寄存器分配 | 从栈式/虚拟寄存器到物理寄存器的分配（先做栈分配，后续线性扫描 / 图着色提升性能分） |
| `opt/` | 优化 | 常量折叠/传播、代数化简、窥孔优化、死代码消除、跳转短路等（`-opt` 打开，增量添加） |

## 与其它模块的接口约定（联调）

后端的输入来自中端 (C)，具体契约见 `docs/backend-design.md` §1：

- **输入**：中端交付的 AST + 符号信息（每个变量引用已解析到「全局标签」或「局部槽位」，
  常量已在编译期求值；作用域/遮蔽/声明先后由 C 处理完）。
- **输出**：向 **stdout** 写 RV32 汇编，可被 `riscv64-*-gcc -march=rv32im -mabi=ilp32` 汇编链接。
- 头文件接口放在 `include/backend/`，实现放在本目录。

## 计划实现顺序

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

骨架阶段，规格已对齐到 ToyC / RV32 / stdin-stdout（见 docs/）。
等待中端 (C) 交付 AST + 符号接口后开始编码。当前目录仅含占位文件。
