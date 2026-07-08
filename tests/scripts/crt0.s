    # 本地测试用的最小启动例程（freestanding）。
    # 调用 main，把返回值（a0）作为 exit 系统调用的退出码。
    # 仅用于本机 qemu-riscv32 验证；评测环境使用其自带的 rv32 工具链与 crt/libc。
    .text
    .globl _start
_start:
    call  main
    li    a7, 93        # __NR_exit
    ecall
