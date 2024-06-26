/*

MIT License

Copyright (c) 2019 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

.include "common/hardware/hwregs.inc"

    .section .boot, "ax", @progbits
    .align 2
    .global flushCache
    .global _reset
    .type _reset, @function

_reset:
    /* set bios memory bus width and speed. */
    li    $t0, (19 << 16) | 0x243f
    sw    $t0, SBUS_DEV2_CTRL

    /* psx.s configures the DRAM controller here, but we are going to do that
       after setting up DEV0/EXP1 as we have to probe a bit in there first. */
    nop
    nop
    nop
    nop

    /* this may be here to let the hardware pick up the new bus settings
       before moving on with the actual code. Also, some tools like IDA
       or even PCSX-Redux use it as a signature to detect this is a PS1
       BIOS file. */
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop

    /* jumping over the interrupt vector. */
    j     _boot

.ascii "OpenBIOS"

    .section .text._boot, "ax", @progbits
    .align 2
    .global _boot
    .type _boot, @function

_boot:
    /* initializing all of the buses now */
    li    $t0, 0x31125
    sw    $t0, SBUS_COM_CTRL

    li    $t0, 0x1f000000
    sw    $t0, SBUS_DEV0_ADDR

    li    $t0, 0x1f802000
    sw    $t0, SBUS_DEV8_ADDR

    /* 8 MB with a 16-bit bus, different value from the default one in psx.s */
    li    $t0, 0x24173f47
    sw    $t0, SBUS_DEV0_CTRL

    li    $t0, 0x200931e1
    sw    $t0, SBUS_DEV4_CTRL

    li    $t0, 0x20843
    sw    $t0, SBUS_DEV5_CTRL

    li    $t0, 0x3022
    sw    $t0, SBUS_DEV1_CTRL

    /* The original code uses 70777 here, but we have
       some debugging routines hitting 0x1f802080,
       beyond the normal range, so we need to extend it,
       to avoid crashes on the real hardware. */
    li    $t0, 0x80777
    sw    $t0, SBUS_DEV8_CTRL

    /* Clear the watchdog. */
    sh    $0, SYS573_WATCHDOG

    /* The 700B01 BIOS uses the following code to determine whether the board
       is an older one with eight 512 KB RAM chips, or a revision D populated
       with two 2 MB chips. The 700A01 BIOS predates such revision and always
       sets the RAM size register to 0xc80. */
    lhu   $t1, SYS573_JAMMA_P2_EXT
    li    $t0, 0xc80
    andi  $t1, 1 << 10
    bnez  $t1, old_board_revision

new_board_revision:
    li    $t0, 0x4788

old_board_revision:
    sw    $t0, RAM_SIZE

    /* clearing out all registers */
    .set push
    .set noat
    move  $1, $0
    .set pop
    move  $2, $0
    move  $3, $0
    move  $4, $0
    move  $5, $0
    move  $6, $0
    move  $7, $0
    move  $8, $0
    move  $9, $0
    move  $10, $0
    move  $11, $0
    move  $12, $0
    move  $13, $0
    move  $14, $0
    move  $15, $0
    move  $16, $0
    move  $17, $0
    move  $18, $0
    move  $19, $0
    move  $20, $0
    move  $21, $0
    move  $22, $0
    move  $23, $0
    move  $24, $0
    move  $25, $0
    move  $26, $0
    move  $27, $0
    move  $28, $0
    move  $29, $0
    move  $30, $0
    move  $31, $0

    jal  flushCache

    /* ensuring cop0 is fully reset */
    mtc0  $0, $7
    nop
    mtc0  $0, $3
    nop
    mtc0  $0, $5
    nop
    mtc0  $0, $6
    nop
    mtc0  $0, $9
    nop
    mtc0  $0, $11
    nop
    mtc0  $0, $12
    nop
    mtc0  $0, $13
    nop

    /* Extra from OpenBIOS, not in the original BIOS:
       Enable cop2, as some games may rely on it being
       enabled as a side effect of the shell running,
       and our replacement shell does not enable it. */
    lui   $t0, 0x4000
    mtc0  $t0, $12
    nop

    /* Now we are ready for a typical crt0.
       The original bios does not do this, most likely
       for speed reasons. It would be more efficient to
       run these loops in RAM instead of the ROM. But
       we have enough code that would rely in all this
       to be set up already before starting, it would
       be a mistake to not do it here. */
    la    $t0, __data_start
    la    $t1, __data_end
    la    $t2, __rom_data_start

    beq   $t0, $t1, data_copy_skip

data_copy:
    lw    $t3, 0($t2)
    sw    $t3, 0($t0)
    addiu $t0, 4
    addiu $t2, 4
    bne   $t0, $t1, data_copy

data_copy_skip:
    la    $t0, __bss_start
    la    $t1, __bss_end

    beq   $t0, $t1, bss_init_skip

bss_init:
    sw    $0, 0($t0)
    addiu $t0, 4
    bne   $t0, $t1, bss_init

bss_init_skip:
    /* technically have to set $gp, but we are not using it, so, not */
    la    $sp, __sp
    move  $fp, $sp

    jal   _ucsdk_start

    li    $t0, 0x1f802080
    li    $t1, 10
    sb    $t1, 0($t0)
    sb    $t1, 1($t0)
stop:
    b     stop
