TEXT zpread(SB), 1, $0
MOVQ RARG, a0+0(FP)
MOVQ $57, RARG
SYSCALL
RET
