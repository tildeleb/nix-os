TEXT oseek(SB), 1, $0
MOVQ RARG, a0+0(FP)
MOVQ $16, RARG
SYSCALL
RET
