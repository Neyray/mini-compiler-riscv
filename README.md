# mini-compiler-riscv

程序语言理论与编译技术 —— 编译系统实践（课程大作业）

一个用 **C++** 实现的小型编译器：将 **ToyC 语言**（C 语言的一个简化子集，具体文法见任务书
与 `docs/toyc-spec.md`）源程序编译为 **RISC-V32 汇编代码**。整体流程遵循经典编译器结构：

```
ToyC 源程序 (.tc)  ← 从标准输入 stdin 读取
   │  ┌──────────── 前端 (B) ────────────┐
   ▼  │ 词法分析 → 语法分析 → AST 构建     │
        └───────────────┬──────────────────┘
                        ▼
        ┌──────────── 中端 (C) ────────────┐
        │ 符号表 → 语义分析 → 类型检查 → 作用域 │
        └───────────────┬──────────────────┘
                        ▼
        ┌──────────── 后端 (D) ────────────┐
        │ RISC-V32 代码生成 → 优化 → 联调     │
        └───────────────┬──────────────────┘
                        ▼
          RISC-V32 汇编 (.s)  → 写入标准输出 stdout
```

> **接口约定（任务书要求）**：编译器从 **stdin** 读取 ToyC 源程序，向 **stdout** 写入
> RISC-V32 汇编。stderr 输出随意。性能测试时会传入 `-opt` 参数表示开启优化（可选择启用优化或忽略）。

## 团队分工

| 成员 | 模块 | 职责 |
| --- | --- | --- |
| A | 报告 / 测试 | 实验报告、整理截图、设计测试用例、协助调试 |
| B | 前端 | 词法分析 + 语法分析（AST 构建） |
| C | 中端 | 符号表 + 语义分析 + 类型检查 + 作用域 |
| D | 后端 | RISC-V32 代码生成 + 优化 + 联调 |

> 各模块接口约定见 `docs/`：语言规格 `toyc-spec.md`、AST/符号→后端契约 `backend-design.md`、
> 联调与实现记录 `integration-notes.md`。

## 目录结构

```
mini-compiler-riscv/
├── README.md              # 本文件（项目总览）
├── LICENSE
├── .gitignore
├── CMakeLists.txt         # 顶层构建入口（CMake）
├── Makefile               # 顶层构建入口（Make，等价于 CMake）
├── docs/                  # 设计文档与分工说明
│   ├── division-of-labor.md
│   ├── toyc-spec.md       # ToyC 语言文法与语义约束（任务书摘录，全员依据）
│   └── backend-design.md  # 后端(D)设计说明 + 前端/中端→后端接口契约
├── include/               # 公共头文件
│   ├── common/            # 跨模块通用定义（错误处理、位置信息等）
│   ├── frontend/          # 前端接口 (B)
│   ├── midend/            # 中端接口 (C)
│   └── backend/           # 后端接口 (D)
├── src/                   # 源码实现
│   ├── frontend/          # (B) 词法/语法分析、AST
│   ├── midend/            # (C) 符号表/语义/类型/作用域
│   ├── backend/           # (D) 代码生成/寄存器分配/优化
│   │   ├── codegen/
│   │   ├── regalloc/
│   │   └── opt/
│   └── main.cpp           # 驱动入口（编译流水线串联，读 stdin / 写 stdout）
├── tests/                 # 测试 (A)
│   ├── cases/functional/  # 功能测试用例源程序 (.tc)
│   ├── expected/          # 期望输出（主函数返回值 / 退出码）
│   └── scripts/           # 测试脚本
└── scripts/               # 构建/运行辅助脚本
```

## 构建

项目使用 **C++（C++20）** 编译，CMake 与 Make 两种方式等价：

```bash
# 方式一：CMake（生成 build/compiler）
cmake -B build -S .
cmake --build build -j

# 方式二：Make（生成 ./compiler）
make
```

## 运行接口（任务书约定）

编译器从 **标准输入** 读取源程序，向 **标准输出** 写出汇编。本地测试用重定向即可：

```bash
./build/compiler < input.tc > output.s          # 功能编译
./build/compiler -opt < input.tc > output.s      # 开启优化（性能测试）
```

## 目标平台工具链

后端生成 **RISC-V32（RV32）** 汇编。本地验证（RV32 ILP32）时可用：

```bash
# 汇编 + 链接为 RV32 可执行文件
riscv64-linux-gnu-gcc -march=rv32im -mabi=ilp32 -static output.s -o a.out
# 运行并检查退出码（程序结果 = main 的返回值，0~255）
qemu-riscv32 ./a.out ; echo "exit code = $?"
```

> 也可使用 `riscv32-unknown-elf-gcc` + `spike`/`pk`。评测系统以 `gcc -O2` 生成代码的运行时间为性能基准。

## 本地回归测试

ToyC 是 C 的子集，可用本机 `gcc` 直接把 `.tc` 当作 C 编译运行，得到每个用例的期望退出码作为
基准；再用本编译器生成 RV32 汇编、经 `qemu-riscv32` 运行，比对退出码。用例见
`tests/cases/functional/`，一键运行：

```bash
make                              # 先构建出 ./compiler
bash tests/scripts/run_tests.sh          # 功能测试
bash tests/scripts/run_tests.sh -opt     # 开启优化后再测一遍
```

> 说明：本机 `riscv64-linux-gnu-gcc` 缺少 rv32 的 libc/crt multilib，测试脚本改用
> `-nostdlib` + `tests/scripts/crt0.s`（最小启动例程，调用 `main` 后以其返回值退出）以
> freestanding 方式链接。评测环境使用其自带 rv32 工具链，本编译器输出为标准汇编，无需该文件。

## 环境

- 开发环境：WSL2 (Ubuntu 22.04)
- 语言：C++（C++20；任务书允许 C++20 / Java 21 / OCaml 5.3.0，本组选择 C++）
- 构建：CMake（4.0.3）+ Make

## 评分构成（任务书）

- 总得分 = 评测得分 × 80% + 实践报告得分 × 20%
- 评测得分 = 功能得分 × 75% + 性能得分 × 25%
- 性能得分以 `gcc -O2` 运行时间为基准，满分封顶（今年提高了性能占比，鼓励后端优化）

## 完成情况

- 前端：词法分析 + 语法分析 + AST（B）
- 中端：符号表 + 语义分析 + 类型检查 + 作用域（C）
- 后端：RISC-V32 代码生成（D）
- 后端优化：寄存器分配、常量折叠/传播、短路跳转、强度削弱、窥孔、叶子函数（D）
- 构建：CMake / Make 双入口，从 `stdin` 读源程序、向 `stdout` 写汇编，识别 `-opt`
- 联调 + 测试（D + A）：42 个功能用例在 `-opt` 与非 `-opt` 下全部通过
- 实验报告（A）：见 `docs/编译系统实践报告.docx`
