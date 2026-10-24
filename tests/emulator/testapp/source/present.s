@ A frame "present" function shaped like the one in Nintendo's graphics
@ library, so CTRPluginFramework's OSD hook finds and hooks it the same way it
@ does in real games. Homebrew built with libctru doesn't contain that code.
@
@ CTRPF searches the code for the instruction pattern below, walks back to
@ the closest "STMFD SP!, {R4-R12, LR}" and hooks that function, calling it as
@ Present(isBottom, bank, leftFb, rightFb, stride, format, swap).

    .arm
    .section .text.NnStylePresent, "ax", %progbits
    .global NnStylePresent
    .type NnStylePresent, %function
NnStylePresent:
    stmfd   sp!, {r4-r12, lr}
    ldr     r4, [sp, #40]           @ stride  (5th argument)
    ldr     r5, [sp, #44]           @ format  (6th)
    ldr     r6, [sp, #48]           @ swap    (7th)
    sub     sp, sp, #16
    str     r4, [sp]
    str     r5, [sp, #4]
    str     r6, [sp, #8]
    bl      PresentImpl
    add     sp, sp, #16
    ldmfd   sp!, {r4-r12, pc}

    @ Never executed: the instruction pattern CTRPF looks for
    .word   0xE1833000              @ ORR   R3, R3, R0
    .word   0xE2044CFF              @ AND   R4, R4, #0xFF00
    .word   0xE3C33CFF              @ BIC   R3, R3, #0xFF00
    .word   0xE1833004              @ ORR   R3, R3, R4
    .word   0xE1824F93              @ STREX R4, R3, [R2]
    .size NnStylePresent, .-NnStylePresent
