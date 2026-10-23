    .text
    .p2align 4
    .globl bedlam2_native_main
    .type bedlam2_native_main, @function
bedlam2_native_main:
    # Save all SysV AMD64 callee-saved general-purpose registers. The legacy
    # engine treats several of them as scratch registers.
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    movq %rsp, %r12
    movq bedlam2_ps5_main_stack_top(%rip), %rsp
    andq $-16, %rsp

    # Make a 16-byte area containing the old i386 argc/argv/envp stack slots.
    # The PS5 frontend currently starts without command-line switches, so the
    # engine sees argc=1 and a null argv pointer; its option loop is skipped.
    subq $16, %rsp
    movl $1, (%rsp)
    movl $0, 4(%rsp)
    movl $0, 8(%rsp)
    movl $1, %eax
    xorl %edx, %edx
    call main_

    movq %r12, %rsp
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size bedlam2_native_main, .-bedlam2_native_main

    .p2align 4
    .globl bedlam2_native_level_clock
    .type bedlam2_native_level_clock, @function
bedlam2_native_level_clock:
    # The timer callback uses the native ABI; isolate the legacy engine's
    # register clobbers at this boundary as well.
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    movq %rsp, %r12
    movq bedlam2_ps5_timer_stack_top(%rip), %rsp
    andq $-16, %rsp
    call level_clock
    movq %r12, %rsp
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size bedlam2_native_level_clock, .-bedlam2_native_level_clock

    .p2align 4
    .globl bedlam2_ps5_cycle_weapon_native
    .type bedlam2_ps5_cycle_weapon_native, @function
bedlam2_ps5_cycle_weapon_native:
    # The converted engine procedure reserves R13-R15 for its mirrored data
    # and text bases. Preserve those native callee-saved registers and RBX,
    # while passing the requested direction through EDI.
    pushq %rbx
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    call bedlam2_ps5_cycle_weapon
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %rbx
    ret
    .size bedlam2_ps5_cycle_weapon_native, .-bedlam2_ps5_cycle_weapon_native

    .p2align 4
    .globl bedlam2_ps5_cycle_robot_native
    .type bedlam2_ps5_cycle_robot_native, @function
bedlam2_ps5_cycle_robot_native:
    # Keep the engine's mirrored data/text bases and native callee-saved state
    # intact while the legacy helper uses its 32-bit register convention.
    pushq %rbx
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    call bedlam2_ps5_cycle_robot
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %rbx
    ret
    .size bedlam2_ps5_cycle_robot_native, .-bedlam2_ps5_cycle_robot_native

    .p2align 4
    .globl bedlam2_ps5_center_view_native
    .type bedlam2_ps5_center_view_native, @function
bedlam2_ps5_center_view_native:
    pushq %rbx
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    call bedlam2_ps5_center_view
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %rbx
    ret
    .size bedlam2_ps5_center_view_native, .-bedlam2_ps5_center_view_native

    .section .note.GNU-stack,"",@progbits
