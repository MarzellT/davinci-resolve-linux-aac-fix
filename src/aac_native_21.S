/* Internal branches/data are position independent so this block can be copied
 * into a private low-address RX mapping. No Resolve code cave is overwritten. */
.text
.global aac_stubs_start,aac_stubs_end,aac_dispatch,aac_lookup,aac_gate1,aac_gate2
.hidden aac_stubs_start,aac_stubs_end,aac_dispatch,aac_lookup,aac_gate1,aac_gate2
.balign 16
aac_stubs_start:
aac_dispatch:
    cmpl $0x61616320,%r15d
    je 1f
    jmp *2f(%rip)
1:  jmp *3f(%rip)
2:  .quad 0x5dd648b
3:  .quad 0x5dd64dc
.balign 16
aac_lookup:
    cmpl $0x61616320,0x18(%rbx)
    je 1f
    movabs $0x297af098,%r8
    mov (%r8),%r8
    jmp *2f(%rip)
1:  lea 0x60(%rbx),%r14
    lea 4f(%rip),%r13
    jmp *3f(%rip)
2:  .quad 0x5dcb2c0
3:  .quad 0x5dcb3a8
.balign 8
4:  .zero 28
    .long 0x61616320
    .long 0x15002
.balign 16
aac_gate1:
    cmpl $0x15002,%ecx
    je 1f
    cmpl $0x15010,%ecx
    je 1f
    jmp *2f(%rip)
1:  jmp *3f(%rip)
2:  .quad 0x5dcb497
3:  .quad 0x5dcb4a3
.balign 16
aac_gate2:
    cmpl $0x15002,%edi
    je 1f
    cmpl $0x15010,%edi
    je 2f
    jmp *3f(%rip)
1:  jmp *4f(%rip)
2:  jmp *5f(%rip)
3:  .quad 0x5dcb4f1
4:  .quad 0x5dcb60f
5:  .quad 0x5dcb5de
aac_stubs_end:
.section .note.GNU-stack,"",@progbits
