/*
 * cv_dl_magic.S
 *
 * Source reconstruction of cv_dl_magic.bin.
 *
 * This is target-side AArch64 ROM-stage code for CV180x/CV181x download
 * flow.  It is never linked into, nor executed by, the x86_64 host utility.
 * The build converts this file into the 128-byte cv_dl_magic.bin payload sent
 * over USB by usb_dl.
 *
 * The blob intentionally branches/calls outside its own 0x80-byte extent
 * (for example 0xa0, 0x110, 0x130, 0x810, 0x830).  Those targets belong to
 * the ROM / early boot execution environment, not to this standalone file.
 *
 * To keep the generated binary byte-identical, this file uses .inst for the
 * exact AArch64 encodings and annotates each instruction with the recovered
 * mnemonic.
 */

    .section .text,"ax",%progbits
    .balign 4
    .global cv_dl_magic_start
    .type cv_dl_magic_start, %function

cv_dl_magic_start:
    /* 0000 */ .inst 0x14000008    /* b      0x20 */
    /* 0004 */ .inst 0xd518c000    /* msr    VBAR_EL1, x0 */
    /* 0008 */ .inst 0xd2a00600    /* mov    x0, #0x300000 */
    /* 000c */ .inst 0xd5181040    /* msr    CPACR_EL1, x0 */
    /* 0010 */ .inst 0x94000040    /* bl     0x110 */
    /* 0014 */ .inst 0x35000040    /* cbnz   w0, 0x1c */
    /* 0018 */ .inst 0x14000000    /* b      0x18 */
    /* 001c */ .inst 0x10003fa0    /* adr    x0, 0x810 */

    /* 0020 */ .inst 0x14000008    /* b      0x40 */
    /* 0024 */ .inst 0xd518c000    /* msr    VBAR_EL1, x0 */
    /* 0028 */ .inst 0xd2a00600    /* mov    x0, #0x300000 */
    /* 002c */ .inst 0xd5181040    /* msr    CPACR_EL1, x0 */
    /* 0030 */ .inst 0x94000040    /* bl     0x130 */
    /* 0034 */ .inst 0x35000040    /* cbnz   w0, 0x3c */
    /* 0038 */ .inst 0x14000000    /* b      0x38 */
    /* 003c */ .inst 0x10003fa0    /* adr    x0, 0x830 */

    /* 0040 */ .inst 0xd53e1100    /* mrs    x0, SCR_EL3 */
    /* 0044 */ .inst 0xb2400c00    /* orr    x0, x0, #0xf */
    /* 0048 */ .inst 0xd51e1100    /* msr    SCR_EL3, x0 */
    /* 004c */ .inst 0xd51e115f    /* msr    CPTR_EL3, xzr */
    /* 0050 */ .inst 0xd53e1000    /* mrs    x0, SCTLR_EL3 */
    /* 0054 */ .inst 0xd28000a1    /* mov    x1, #0x5 */
    /* 0058 */ .inst 0x8a210000    /* bic    x0, x0, x1 */
    /* 005c */ .inst 0xd51e1000    /* msr    SCTLR_EL3, x0 */
    /* 0060 */ .inst 0x14000010    /* b      0xa0 */

    /* 0064 */ .inst 0xd51cc000    /* msr    VBAR_EL2, x0 */
    /* 0068 */ .inst 0xd2867fe0    /* mov    x0, #0x33ff */
    /* 006c */ .inst 0xd51c1140    /* msr    CPTR_EL2, x0 */
    /* 0070 */ .inst 0xd53c1000    /* mrs    x0, SCTLR_EL2 */
    /* 0074 */ .inst 0xd28000a1    /* mov    x1, #0x5 */
    /* 0078 */ .inst 0x8a210000    /* bic    x0, x0, x1 */
    /* 007c */ .inst 0xd51c1000    /* msr    SCTLR_EL2, x0 */

cv_dl_magic_end:
    .size cv_dl_magic_start, . - cv_dl_magic_start
