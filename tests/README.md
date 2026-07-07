# 测试 (Tests) —— 负责人 A

- `cases/functional/` —— 功能测试用例源程序（SysY / C 子集）。
- `expected/` —— 每个用例的期望输出（退出码 / stdout）。
- `scripts/` —— 自动化测试脚本：编译用例 → 生成 `.s` → 用 `riscv64-*-gcc` 汇编链接
  → `qemu-riscv64` 运行 → 比对期望输出。

> 占位目录，待用例设计与联调阶段填充。
