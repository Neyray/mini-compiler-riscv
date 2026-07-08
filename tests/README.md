# 测试 (Tests) —— 负责人 A

- `cases/functional/` —— 功能测试用例源程序（**ToyC**，`.tc` 文件）。
- `expected/` —— 每个用例的期望输出（**主函数返回值 / 进程退出码，0~255**）。
- `scripts/` —— 自动化测试脚本：`compiler < case.tc > case.s`
  → 用 `riscv64-linux-gnu-gcc -march=rv32im -mabi=ilp32 -static case.s -o case.out` 汇编链接
  → `qemu-riscv32 ./case.out` 运行 → 比对退出码是否与期望一致。

> 判定方式（任务书）：程序结果以 `main` 的返回值为准。期望值可用等价 C 程序
> `gcc case.c -o ref && ./ref; echo $?` 生成，再与编译器产物的退出码比对。
> 性能测试以 `gcc -O2` 生成代码的运行时间为基准。

> 现状：`cases/functional/` 已含 21 个功能用例，`scripts/run_tests.sh [-opt]` 以本机 gcc 为基准
> 自动判分（21/21 通过）；`cases/perf/` + `scripts/perf_compare.sh` 用于与 `gcc -O2` 对比性能；
> `scripts/crt0.s` 为本地 freestanding 链接用的最小启动例程。可按需继续扩充用例。
