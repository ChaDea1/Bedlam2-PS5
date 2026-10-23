    .text

    # Adapt the engine's four-byte cdecl argument slots to the SysV AMD64
    # argument registers. Preserve legacy EDI/ESI and the host's R12 value.
    .macro BEDLAM2_LEGACY_THUNK thunk, target, argc
    .p2align 4
    .globl \thunk
    .type \thunk, @function
\thunk:
    pushq %rdi
    pushq %rsi
    pushq %r12
    movq %rsp, %r12
    andq $-16, %rsp
    .if \argc > 0
    movl 32(%r12), %edi
    .endif
    .if \argc > 1
    movl 36(%r12), %esi
    .endif
    .if \argc > 2
    movl 40(%r12), %edx
    .endif
    .if \argc > 3
    movl 44(%r12), %ecx
    .endif
    .if \argc > 4
    movl 48(%r12), %r8d
    .endif
    .if \argc > 5
    movl 52(%r12), %r9d
    .endif
    .ifc \target, bedlam2_ps5_fseek_legacy
    movslq 36(%r12), %rsi
    .endif
    .ifc \target, printf
    xorl %eax, %eax
    .endif
    # Native imports use eagerly resolved GOT slots. ELF lazy PLT resolution
    # is not a usable fallback in the native title's loader context.
    call *\target@GOTPCREL(%rip)
    movq %r12, %rsp
    popq %r12
    popq %rsi
    popq %rdi
    ret
    .size \thunk, .-\thunk
    .endm

    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_clear_screen, clear_screen, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_copy_buffer_to_screen_and_unlock, copy_buffer_to_screen_and_unlock, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_copy_screen_to_buffer, copy_screen_to_buffer, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_correct_screen_pos, correct_screen_pos, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_draw_map_and_game_screen, draw_map_and_game_screen, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_exit, exit, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fclose, bedlam2_ps5_fclose_legacy, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fflush, bedlam2_ps5_fflush_legacy, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fopen, bedlam2_ps5_fopen_legacy, 2
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fread, bedlam2_ps5_fread_legacy, 4
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_free, bedlam2_ps5_free, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_free_win, free_win, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fseek, bedlam2_ps5_fseek_legacy, 3
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_ftell, bedlam2_ps5_ftell_legacy, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fwrite, bedlam2_ps5_fwrite_legacy, 4
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_get_screen_buffer_ptr, get_screen_buffer_ptr, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_getcwd, bedlam2_ps5_getcwd_legacy, 2
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_hide_cursor, hide_cursor, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_load_music, load_music, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_load_raw, load_raw, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_malloc, bedlam2_ps5_malloc, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_mkdir, bedlam2_ps5_mkdir_legacy, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_play_music, play_music, 4
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_play_sound, play_sound, 4
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_play_wav_music, play_wav_music, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_printf, printf, 6
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_putchar, putchar, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_puts, puts, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_redraw, redraw, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_redraw_copy_buff_to_screen_and_unlock, redraw_copy_buff_to_screen_and_unlock, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_set_palette, set_palette, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_set_volume, set_volume, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_show_cursor, show_cursor, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_stop_music, stop_music, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_stop_wav_music, stop_wav_music, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_swap_palette_with_animation, swap_palette_with_animation, 2
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_unlock_surface_and_wait, unlock_surface_and_wait, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_wait_palette_animation, wait_palette_animation, 1
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_sdl_events, SDL_events, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_fill_player_name, bedlam2_ps5_fill_player_name, 2
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_legacy_shop_loop_started, bedlam2_ps5_shop_loop_started, 0
    BEDLAM2_LEGACY_THUNK bedlam2_ps5_keyboard_status_bridge, bedlam2_ps5_keyboard_status, 0

    # The legacy Watcom runtime passes DOS file handles and buffers in registers.
    # These adapters keep the original register-call surface while using SysV C++.
    .p2align 4
    .globl bedlam2_ps5_dos_lseek_bridge
    .type bedlam2_ps5_dos_lseek_bridge, @function
bedlam2_ps5_dos_lseek_bridge:
    pushq %rdi
    pushq %rcx
    pushq %r12
    movq %rsp, %r12
    andq $-16, %rsp
    movl %eax, %edi
    movzbl %bl, %esi
    call bedlam2_ps5_dos_lseek
    movq %r12, %rsp
    popq %r12
    popq %rcx
    popq %rdi
    ret
    .size bedlam2_ps5_dos_lseek_bridge, .-bedlam2_ps5_dos_lseek_bridge

    .p2align 4
    .globl bedlam2_ps5_dos_qwrite_bridge
    .type bedlam2_ps5_dos_qwrite_bridge, @function
bedlam2_ps5_dos_qwrite_bridge:
    pushq %rdi
    pushq %rsi
    pushq %rcx
    pushq %r12
    movq %rsp, %r12
    andq $-16, %rsp
    movl %eax, %edi
    movl %edx, %esi
    movl %ebx, %edx
    call bedlam2_ps5_dos_write
    movq %r12, %rsp
    popq %r12
    popq %rcx
    popq %rsi
    popq %rdi
    ret
    .size bedlam2_ps5_dos_qwrite_bridge, .-bedlam2_ps5_dos_qwrite_bridge

    .p2align 4
    .globl bedlam2_ps5_dos_isatty_bridge
    .type bedlam2_ps5_dos_isatty_bridge, @function
bedlam2_ps5_dos_isatty_bridge:
    pushq %rdi
    pushq %rsi
    pushq %rcx
    pushq %rdx
    pushq %r12
    movq %rsp, %r12
    andq $-16, %rsp
    movl %eax, %edi
    call bedlam2_ps5_dos_isatty
    movq %r12, %rsp
    popq %r12
    popq %rdx
    popq %rcx
    popq %rsi
    popq %rdi
    ret
    .size bedlam2_ps5_dos_isatty_bridge, .-bedlam2_ps5_dos_isatty_bridge

    .section .note.GNU-stack,"",@progbits
