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

@ Nintendo's GSPGPU RegisterInterruptRelayQueue (command 0x00130042), word
@ for word as CTRPF expects to find it. CTRPF hooks its "svc 0x32" to learn
@ the GPU interrupt event and shared memory, and refuses to start without it.
@
@ Result NnStyleRegisterInterruptRelayQueue(Handle *session, Handle event,
@         u32 flags, Handle *outSharedMem, u32 *outThreadId)
    .section .text.NnStyleRegisterInterruptRelayQueue, "ax", %progbits
    .global NnStyleRegisterInterruptRelayQueue
    .type NnStyleRegisterInterruptRelayQueue, %function
NnStyleRegisterInterruptRelayQueue:
    .word   0xE92D4070              @ push  {r4-r6, lr}
    .word   0xE1A06003              @ mov   r6, r3            (outSharedMem)
    .word   0xE59D5010              @ ldr   r5, [sp, #16]     (outThreadId)
    .word   0xEE1D3F70              @ mrc   p15, 0, r3, c13, c0, 3
    .word   0xE2834080              @ add   r4, r3, #0x80     (IPC buffer)
    .word   0xE59F3034              @ ldr   r3, =0x00130042
    .word   0xE3A0C000              @ mov   ip, #0
    .word   0xE584100C              @ str   r1, [r4, #12]     (event)
    .word   0xE9841004              @ stmib r4, {r2, ip}      (flags, handle descriptor)
    .word   0xE5843000              @ str   r3, [r4]
    .word   0xE5900000              @ ldr   r0, [r0]
    .word   0xEF000032              @ svc   0x32              (SendSyncRequest)
    .word   0xE2101102              @ ands  r1, r0, #0x80000000
    .word   0x4A000004              @ bmi   done
    .word   0xE5940008              @ ldr   r0, [r4, #8]
    .word   0xE5850000              @ str   r0, [r5]          (thread id)
    .word   0xE5940010              @ ldr   r0, [r4, #16]
    .word   0xE5860000              @ str   r0, [r6]          (shared memory handle)
    .word   0xE5940004              @ ldr   r0, [r4, #4]      (result)
    .word   0xE8BD8070              @ done: pop {r4-r6, pc}
    .word   0x00130042
    .size NnStyleRegisterInterruptRelayQueue, .-NnStyleRegisterInterruptRelayQueue
