    .text
    .globl main_
    .type main_,@function
main_:
    cmpl $1,8(%rsp)
    jne .Lbad_args
    cmpl $0,12(%rsp)
    jne .Lbad_args
    cmpl $0,16(%rsp)
    jne .Lbad_args
    pushq %rbx
    xorl %edi,%edi
    movq %rsp,%rsi
    call check_engine_stack
    popq %rbx
    subq $160,%rsp
    movq %rsp,%rax
    leaq -4(%rsp),%rsp
    movl $128,(%rsp)
    leaq -4(%rsp),%rsp
    movl %eax,(%rsp)
    call bedlam2_ps5_legacy_getcwd
    addq $8,%rsp
    testq %rax,%rax
    jz .Lbad_cwd
    cmpb $47,(%rax)
    jne .Lbad_cwd
    leaq -4(%rsp),%rsp
    movl %eax,(%rsp)
    call bedlam2_ps5_legacy_mkdir
    addq $4,%rsp
    testl %eax,%eax
    jne .Lbad_cwd
    addq $160,%rsp
    # Legacy procedures clobber these; the native entry must restore them.
    xorl %ebx,%ebx
    xorl %ebp,%ebp
    xorl %r13d,%r13d
    xorl %r14d,%r14d
    xorl %r15d,%r15d
    movl $0x1234,%eax
    ret
.Lbad_cwd:
    addq $160,%rsp
.Lbad_args:
    movl $-1,%eax
    ret
    .size main_,.-main_

    .globl level_clock
    .type level_clock,@function
level_clock:
    pushq %rbx
    movl $1,%edi
    movq %rsp,%rsi
    call check_engine_stack
    popq %rbx
    ret
    .size level_clock,.-level_clock

    .globl check_native_main_abi
    .type check_native_main_abi,@function
check_native_main_abi:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8,%rsp
    movl $11,%ebx
    movl $22,%ebp
    movl $33,%r12d
    movl $44,%r13d
    movl $55,%r14d
    movl $66,%r15d
    call bedlam2_native_main
    cmpl $0x1234,%eax
    jne .Labi_failure
    cmpl $11,%ebx
    jne .Labi_failure
    cmpl $22,%ebp
    jne .Labi_failure
    cmpl $33,%r12d
    jne .Labi_failure
    cmpl $44,%r13d
    jne .Labi_failure
    cmpl $55,%r14d
    jne .Labi_failure
    cmpl $66,%r15d
    jne .Labi_failure
    xorl %eax,%eax
    jmp .Labi_return
.Labi_failure:
    movl $1,%eax
.Labi_return:
    addq $8,%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size check_native_main_abi,.-check_native_main_abi
    .section .note.GNU-stack,"",@progbits
