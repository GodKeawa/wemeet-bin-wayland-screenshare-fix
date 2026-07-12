default rel

section .text   align = 1
extern dlsym
global getenv, ptr_src

getenv:
    ; If `rdi ^ [rsp] == (0x0001 | 0x0002) << 48`, then it is a hook call.
    mov     rax,    [rsp]
    xor     rax,    rdi
    shl     rax,    16
    test    rax,    rax
    jne     .real_call
    mov     rax,    rdi
    sar     rax,    48
    cmp     eax,    0
    jg      .hook_call
.real_call:
    mov     rax,    [ptr_call]
    test    rax,    rax
    jne     .cached_call
    push    rdi
    xor     edi,    edi
    dec     rdi                 ; RTLD_NEXT
    lea     rsi,    [name_call]
    call    [dlsym wrt ..got]
    mov     [ptr_call], rax
    pop     rdi
.cached_call:
    jmp     rax                 ; real getenv()
.hook_call:
    cmp     eax,    2
    jg      .return             ; invalid hook call
    je      .flip
    
    ;   case `eax == 1`: A hook call from `libscreen_share_module.so`.
    ;                    Save and copy the frame into our private buffer.
    ;                    INPUT: rsi=src, r11=frame_struct
    ;                    KEEP: r11
    
    ; Safety check: verify if rsi is mapped
    push    r11
    push    rcx
    mov     rax,    26                  ; sys_msync
    mov     rdi,    rsi                 ; addr
    mov     rsi,    1                   ; len
    mov     rdx,    1                   ; MS_ASYNC
    syscall
    mov     rsi,    rdi                 ; restore rsi
    pop     rcx
    pop     r11
    cmp     rax,    -12                 ; -ENOMEM
    je      .return                     ; if unmapped, skip

    mov     [ptr_src], rsi              ; save original src pointer
    
    ; Calculate total pixels = width * height
    mov     rax,    [r11]
    mov     edx,    eax                 ; edx = width
    shr     rax,    32                  ; rax = height
    imul    eax,    edx                 ; eax = total pixels
    
    ; Setup copy pointers
    push    rsi
    push    rdi
    lea     rdi,    [ptr_privateres]
    
    mov     r8d,    eax                 ; r8d = total pixels
    
%ifdef SWAP_COLORS
    ; AVX2 copy and flip (original author's channel swap)
    vmovdqu         ymm8,   yword [shuf_mask]
    
    mov     ecx,    r8d
    and     r8d,    0x3f
    shr     ecx,    6
    test    ecx,    ecx
    jz      .loop_64px_end
.loop_64px:
    vmovdqu         ymm0,   yword [rsi+0x00]
    vmovdqu         ymm1,   yword [rsi+0x20]
    vmovdqu         ymm2,   yword [rsi+0x40]
    vpshufb         ymm0,   ymm0,   ymm8
    vpshufb         ymm1,   ymm1,   ymm8
    vpshufb         ymm2,   ymm2,   ymm8
    vmovdqu         yword [rdi+0x00], ymm0
    vmovdqu         yword [rdi+0x20], ymm1
    vmovdqu         yword [rdi+0x40], ymm2
    vmovdqu         ymm3,   yword [rsi+0x60]
    vmovdqu         ymm4,   yword [rsi+0x80]
    vpshufb         ymm3,   ymm3,   ymm8
    vpshufb         ymm4,   ymm4,   ymm8
    vmovdqu         yword [rdi+0x60], ymm3
    vmovdqu         yword [rdi+0x80], ymm4
    vmovdqu         ymm5,   yword [rsi+0xa0]
    vmovdqu         ymm6,   yword [rsi+0xc0]
    vmovdqu         ymm7,   yword [rsi+0xe0]
    vpshufb         ymm5,   ymm5,   ymm8
    vpshufb         ymm6,   ymm6,   ymm8
    vpshufb         ymm7,   ymm7,   ymm8
    vmovdqu         yword [rdi+0xa0], ymm5
    vmovdqu         yword [rdi+0xc0], ymm6
    vmovdqu         yword [rdi+0xe0], ymm7
    add     rsi,    0x100
    add     rdi,    0x100
    dec     ecx
    jne     .loop_64px
.loop_64px_end:
    mov     ecx,    r8d
    and     r8d,    0x7
    shr     ecx,    3
    jecxz   .loop_8px_end
.loop_8px:
    vmovdqu         ymm0,   yword [rsi]
    vpshufb         ymm0,   ymm0,   ymm8
    vmovdqu         yword [rdi], ymm0
    add     rsi,    0x20
    add     rdi,    0x20
    dec     ecx
    jne     .loop_8px
.loop_8px_end:
    test    r8d,    r8d
    je      .loop_1px_end
.loop_1px:
    mov     eax,    dword [rsi]
    mov     ecx,    eax
    shr     ecx,    16
    xor     ecx,    eax
    and     ecx,    0xff
    xor     eax,    ecx
    shl     ecx,    16
    xor     eax,    ecx
    mov     dword [rdi], eax
    add     rsi,    4
    add     rdi,    4
    dec     r8d
    jne     .loop_1px
.loop_1px_end:
%else
    ; Straight AVX2 copy (No Swap)
    mov     ecx,    r8d
    and     r8d,    0x3f
    shr     ecx,    6
    test    ecx,    ecx
    jz      .loop_64px_end
.loop_64px:
    vmovdqu         ymm0,   yword [rsi+0x00]
    vmovdqu         ymm1,   yword [rsi+0x20]
    vmovdqu         ymm2,   yword [rsi+0x40]
    vmovdqu         yword [rdi+0x00], ymm0
    vmovdqu         yword [rdi+0x20], ymm1
    vmovdqu         yword [rdi+0x40], ymm2
    vmovdqu         ymm3,   yword [rsi+0x60]
    vmovdqu         ymm4,   yword [rsi+0x80]
    vmovdqu         yword [rdi+0x60], ymm3
    vmovdqu         yword [rdi+0x80], ymm4
    vmovdqu         ymm5,   yword [rsi+0xa0]
    vmovdqu         ymm6,   yword [rsi+0xc0]
    vmovdqu         ymm7,   yword [rsi+0xe0]
    vmovdqu         yword [rdi+0xa0], ymm5
    vmovdqu         yword [rdi+0xc0], ymm6
    vmovdqu         yword [rdi+0xe0], ymm7
    add     rsi,    0x100
    add     rdi,    0x100
    dec     ecx
    jne     .loop_64px
.loop_64px_end:
    mov     ecx,    r8d
    and     r8d,    0x7
    shr     ecx,    3
    jecxz   .loop_8px_end
.loop_8px:
    vmovdqu         ymm0,   yword [rsi]
    vmovdqu         yword [rdi], ymm0
    add     rsi,    0x20
    add     rdi,    0x20
    dec     ecx
    jne     .loop_8px
.loop_8px_end:
    test    r8d,    r8d
    je      .loop_1px_end
.loop_1px:
    mov     eax,    dword [rsi]
    mov     dword [rdi], eax
    add     rsi,    4
    add     rdi,    4
    dec     r8d
    jne     .loop_1px
.loop_1px_end:
%endif

    pop     rdi
    pop     rsi
.return:
    ret

.flip:
    ;   case `eax == 2`: A hook call from `libxcast.so`.
    ;                    Check the address of src, and copy from our private buffer.
    ;                    INPUT: rbp=src, rbx=dst, r14d=stride (bytes), r15d=height
    ;                    SAFE: rdi, rsi, rcx, r12, r13, r14, r15
    ;                    OUTPUT: al=1 if mismatch
    ;                            return to `[rsp]+0xbe` if match
    
    ; Safety check: verify if rbp is mapped
    push    r11
    push    rcx
    mov     rax,    26                  ; sys_msync
    mov     rdi,    rbp                 ; addr
    mov     rsi,    1                   ; len
    mov     rdx,    1                   ; MS_ASYNC
    syscall
    pop     rcx
    pop     r11
    cmp     rax,    -12                 ; -ENOMEM
    je      .skip_copy                  ; if unmapped, skip to prevent crash!

    cmp     [ptr_src], rbp
    je      .do_copy
    mov     al,     1
    ret

.do_copy:
    mov     eax,    r14d
    imul    eax,    r15d                ; eax = total bytes to copy
    
    push    rsi
    push    rdi
    push    rcx
    
    mov     ecx,    eax
    lea     rsi,    [ptr_privateres]
    mov     rdi,    rbx
    rep movsb
    
    pop     rcx
    pop     rdi
    pop     rsi
    
    xor     eax,    eax                 ; return al = 0
    add     qword [rsp], 0xbe
    ret

.skip_copy:
    xor     eax,    eax                 ; return al = 0 (skip copy)
    add     qword [rsp], 0xbe
    ret

section .rodata align = 1
name_call   db      "getenv", 0
shuf_mask   db      2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15
            db      2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15

section .bss
ptr_call        resq    1
ptr_src         resq    1
ptr_privateres  resb    67108864            ; 64MB buffer supporting up to 8K resolutions
