#!/usr/bin/env bash
# 性能对比：本编译器(-opt) 生成的 RV32 代码 vs gcc -O2 生成的 RV32 代码，
# 二者同在 qemu-riscv32 上运行、比对退出码与耗时。
# 评分中性能得分 = min(1, 基准时间/实际时间)，此处“基准”即 gcc -O2。
#
# 用法：tests/scripts/perf_compare.sh [用例.tc]     默认 tests/cases/perf/prime.tc

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COMPILER="$ROOT/compiler"
CRT="$ROOT/tests/scripts/crt0.s"
TC="${1:-$ROOT/tests/cases/perf/prime.tc}"
RV="riscv64-linux-gnu-gcc -march=rv32im -mabi=ilp32 -static -nostdlib -nostartfiles"

"$COMPILER" -opt < "$TC" > /tmp/perf_mine.s   || { echo "compile failed"; exit 1; }
$RV "$CRT" /tmp/perf_mine.s -o /tmp/perf_mine.elf || { echo "link mine failed"; exit 1; }
$RV -O2 "$CRT" -x c "$TC" -o /tmp/perf_ref.elf    || { echo "link ref failed"; exit 1; }

qemu-riscv32 /tmp/perf_mine.elf; echo "mine exit=$?"
qemu-riscv32 /tmp/perf_ref.elf;  echo "ref  exit=$?"

measure() {
    local elf="$1" best=999 t
    for _ in 1 2 3; do
        t=$( { /usr/bin/time -f '%e' qemu-riscv32 "$elf"; } 2>&1 >/dev/null | tail -1 )
        if awk "BEGIN{exit !($t < $best)}"; then best=$t; fi
    done
    echo "$best"
}

MINE=$(measure /tmp/perf_mine.elf)
REF=$(measure /tmp/perf_ref.elf)
echo "mine(-opt) best = ${MINE}s"
echo "gcc -O2    best = ${REF}s"
awk "BEGIN{printf \"perf ratio (min(1, ref/mine)) = %.2f\n\", (($REF/$MINE)>1?1:($REF/$MINE))}"
