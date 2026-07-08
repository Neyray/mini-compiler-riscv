# 联调整合说明（后端 D 记录）

本文件记录把前端（B）与中端（C）的交付代码接入本仓库时所做的调整，以及后端（D）的实现要点，
供实验报告与后续维护参考。

## 1. 目录落位

| 交付物 | 落位 |
| --- | --- |
| B：`AST.h` / `Lexer.h` / `Parser.h` | `include/frontend/` |
| B：`AST.cpp` / `Lexer.cpp` / `Parser.cpp` | `src/frontend/` |
| C：`SymbolTable.h` / `SemanticAnalyzer.h` / `CheckedAST.h` | `include/midend/` |
| C：`SymbolTable.cpp` / `SemanticAnalyzer.cpp` | `src/midend/` |
| D：`CodeGen.h` | `include/backend/` |
| D：`CodeGen.cpp` | `src/backend/codegen/` |
| 驱动 `main.cpp`（串联前端→中端→后端） | `src/` |

原前端压缩包内的调试版 `main.cpp`（只打印 AST）与 C 的示例 `main_with_semantic.cpp` 未采用，
统一由 `src/main.cpp` 作为流水线入口：读 stdin → 词法/语法 → 语义检查 → 代码生成 → 写 stdout，
识别 `-opt` 参数。头文件用扁平包含（`#include "AST.h"` 等），构建时把 `include/` 下各模块子目录
都加入搜索路径。

## 2. 对前端（B）代码的修正

联调时发现前端交付的两处问题会导致无法正确编译，已修正（仅改实现，未改对外 AST 结构，
中端/后端接口不受影响）：

1. **`Lexer.cpp` 块注释处理死循环**：原 `skipComment` 对 `/* ... */` 的结束判断有误，遇到
   形如 `/* 任意内容 */` 时会陷入死循环；且一次 `nextToken` 只跳过一段注释，连续两段注释或
   注释紧邻空白时无法全部跳过。改为：正确成对识别 `//` 与 `/* */`，并在 `nextToken` 中用
   “跳空白 + 跳注释”循环直到无可跳过为止。

2. **`Parser.cpp` 记号读取错位**：原解析器用 `current`/`peek` 双记号，但取词与前瞻的约定在
   各函数间不一致（分派函数看 `current`、`expect`/表达式看 `peek`），实际连
   `int main(){ return 0; }` 都会解析错误（例如把 `return 0;` 读成 `return;`）。已将解析器改为
   **先一次性把 Lexer 全部记号读入缓冲区、再用下标前瞻** 的写法，逻辑等价于原递归下降 +
   优先级爬升，产出的 AST 结点类型与字段完全不变。顺带把整数字面量用 `long long` 解析后截断
   到 32 位，避免 `-2147483648` 这类边界值触发 `std::stoi` 越界异常（`-` 由一元运算处理）。

`Parser.h` 私有成员相应从 `current/peek` 改为记号缓冲区 + 下标；`Lexer.h` 增加 `peekNext` 与把
`skipComment` 改为返回 `bool`。公开接口（`Parser(Lexer&)`、`parseCompUnit()`、`Lexer::nextToken()`）
保持不变。

## 3. 对中端（C）代码的修正

中端代码基本可直接使用，仅做两处小调整：

1. **全局普通变量的静态初值**：原 `checkGlobalDecl` 只对 `const` 做编译期求值。后端把非 `const`
   全局变量放入 `.data` 需要静态初值，故对全局声明统一做编译期求值并写入符号（`hasConstValue`）。
   这与 C 语言“全局初始化式须为常量表达式”的语义一致，不影响原有检查。

2. **`int` 函数“所有路径都要有返回值”的判定放宽**：原 `stmtAlwaysReturns` 对 `while` 一律按
   “不必然返回”处理，会把 `int f(){ while(1){ ... return x; } }` 这类合法写法误判为缺少返回而报错。
   改为：当 `while` 条件为编译期非零常量时视为已覆盖所有路径（非常量条件仍保守处理）。这只会
   减少**误报**，不会漏掉真正缺少返回的错误。

## 4. 后端（D）实现要点

- **目标**：RV32IM / ilp32，AST 直接翻译（tree-walking）。表达式结果统一落 `a0`。
- **栈帧**：被调用者保存 `ra`/`s0`，`s0` 作帧指针。函数体内 **`sp` 保持不变**（仅传递 >8 个
  实参时临时调整），因此每个调用点 `sp` 天然 16 字节对齐。栈上局部与临时槽数由一趟预分析
  预估，帧大小向上取整到 16。
- **寄存器分配（性能优化）**：局部变量与形参优先分配到被调用者保存寄存器 `s1..s11`（跨函数调用
  自动保持，无需内存读写），超过 11 个的部分回落到栈槽；用到的 `s` 寄存器在序言/尾声成对
  保存恢复。表达式二元运算的左操作数在「右子树不含函数调用且深度 < 6」时暂存于 `t0..t5`，
  否则溢出到栈临时槽以保证跨调用安全（`containsCall` 分析决定）。这样循环计数器/累加器等热变量
  常驻寄存器，显著减少访存。**叶子函数**（不含任何调用）省去 `ra` 的保存与恢复。
- **变量解析**：后端自带作用域栈完成名字解析（同名遮蔽、块作用域）。常量内联为立即数不占存储；
  全局变量走 `.data` + `la`/`lw`/`sw`；局部/形参走 `s` 寄存器或栈槽。形参前 8 个来自 `a0..a7`、
  其余来自调用者栈。
- **控制流**：`if`/`while` 的条件用 `genCond` 直接生成到目标标签的分支（跳转短路），关系运算融合为
  `blt/bge/beq/bne`；`&&`/`||` 按短路语义生成；`break`/`continue` 用循环标签栈。
- **函数调用**：实参从左到右求值后经栈槽装入 `a0..a7`，超过 8 个的溢出到栈（并临时保持 16 对齐）；
  支持递归。
- **优化（`-opt`）**：常量折叠/传播（含全局常量内联）、乘 2 的幂强度削弱为移位、以及删除
  `mv x,x` 与“跳转到紧邻标签”的窥孔清理。除法不做移位化简（有符号截断除法与算术右移对负数不等价）。

## 5. 构建与测试

- 构建：`cmake -B build -S . && cmake --build build`（产物 `build/compiler`）；或 `make`（产物 `./compiler`）。
- 功能测试：`bash tests/scripts/run_tests.sh [-opt]`。以本机 `gcc` 把 `.tc` 当 C 编译运行得到期望
  退出码，与本编译器产物在 `qemu-riscv32` 上的退出码逐一比对。当前 21 个功能用例在 `-opt` 与
  非 `-opt` 下均全部通过。
- 性能对比：`bash tests/scripts/perf_compare.sh [用例.tc]`（默认 `tests/cases/perf/prime.tc`）。
  本编译器 `-opt` 产物与 `gcc -O2` 产物同在 `qemu-riscv32` 上运行比对。在素数计数基准上，
  两者结果一致，耗时约为 `gcc -O2` 的 1.6 倍（性能得分 `min(1, 基准/实际) ≈ 0.63`）。
