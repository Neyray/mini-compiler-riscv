#!/usr/bin/env bash
# ToyC 编译器功能回归测试。
#
# 判定方式（任务书）：程序结果以 main 的返回值（进程退出码）为准。
# 由于 ToyC 是 C 的子集，用本机 gcc 直接把 .tc 当作 C 编译运行，得到期望退出码；
# 再用本编译器生成 RV32 汇编，经 riscv64-linux-gnu-gcc 汇编链接、qemu-riscv32 运行，
# 比对两者退出码是否一致。
#
# 用法：tests/scripts/run_tests.sh [-opt]

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COMPILER="$ROOT/compiler"
CASES="$ROOT/tests/cases/functional"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

OPTFLAG=""
if [ "${1:-}" = "-opt" ]; then
    OPTFLAG="-opt"
fi

RVGCC="riscv64-linux-gnu-gcc"
# 本机 rv32 交叉工具链缺少 rv32 的 libc/crt multilib，故用 -nostdlib + 自带 crt0
# 以 freestanding 方式链接（ToyC 无 I/O，结果只看 main 退出码）。
CRT0="$ROOT/tests/scripts/crt0.s"
RVFLAGS="-march=rv32im -mabi=ilp32 -static -nostdlib -nostartfiles"

if [ ! -x "$COMPILER" ]; then
    echo "compiler not built: $COMPILER" >&2
    exit 1
fi

pass=0
fail=0

for tc in "$CASES"/*.tc; do
    name="$(basename "$tc")"

    # 期望值：本机 gcc 把 .tc 当 C 编译运行。
    if ! gcc -w -x c "$tc" -o "$WORK/ref" >/dev/null 2>&1; then
        echo "SKIP  $name (gcc reference build failed)"
        continue
    fi
    "$WORK/ref"
    expected=$?

    # 实际值：本编译器 -> RV32 汇编 -> 汇编链接 -> qemu 运行。
    if ! "$COMPILER" $OPTFLAG < "$tc" > "$WORK/out.s" 2> "$WORK/err.txt"; then
        echo "FAIL  $name (compiler error: $(cat "$WORK/err.txt"))"
        fail=$((fail + 1))
        continue
    fi
    if ! $RVGCC $RVFLAGS "$CRT0" "$WORK/out.s" -o "$WORK/out.elf" >"$WORK/as.txt" 2>&1; then
        echo "FAIL  $name (assemble/link error: $(cat "$WORK/as.txt"))"
        fail=$((fail + 1))
        continue
    fi
    qemu-riscv32 "$WORK/out.elf"
    actual=$?

    if [ "$actual" -eq "$expected" ]; then
        echo "PASS  $name (=$actual)"
        pass=$((pass + 1))
    else
        echo "FAIL  $name (expected $expected, got $actual)"
        fail=$((fail + 1))
    fi
done

echo "----------------------------------------"
echo "opt='${OPTFLAG:-none}'  passed $pass  failed $fail"
[ "$fail" -eq 0 ]
