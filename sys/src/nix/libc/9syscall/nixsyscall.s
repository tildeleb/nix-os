TEXT nixsyscall(SB), 1, $0
MOVQ RARG, a0+0(FP)
MOVQ $56, RARG
SYSCALL
RET
