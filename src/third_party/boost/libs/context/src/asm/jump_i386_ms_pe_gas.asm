/*
            Copyright Oliver Kowalke 2009.
            Copyright Thomas Sailer 2013.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)
*/

/*************************************************************************************
*  --------------------------------------------------------------------------------- *
*  |    0    |    1    |    2    |    3    |    4    |    5    |    6    |    7    | *
*  --------------------------------------------------------------------------------- *
*  |    0h   |   04h   |   08h   |   0ch   |   010h  |   014h  |   018h  |   01ch  | *
*  --------------------------------------------------------------------------------- *
*  | fc_mxcsr|fc_x87_cw| fc_strg |fc_deallo|  limit  |   base  |  fc_seh |   EDI   | *
*  --------------------------------------------------------------------------------- *
*  --------------------------------------------------------------------------------- *
*  |    8    |    9    |   10    |    11   |    12   |    13   |    14   |    15   | *
*  --------------------------------------------------------------------------------- *
*  |   020h  |  024h   |  028h   |   02ch  |   030h  |   034h  |   038h  |   03ch  | *
*  --------------------------------------------------------------------------------- *
*  |   ESI   |   EBX   |   EBP   |   EIP   |    to   |   data  |  EH NXT |SEH HNDLR| *
*  --------------------------------------------------------------------------------- *
**************************************************************************************/

.file	"jump_i386_ms_pe_gas.asm"
.text
.p2align 4,,15

/* mark as using no unregistered SEH handlers */
.globl	@feat.00
.def	@feat.00;	.scl	3;	.type	0;	.endef
.set    @feat.00,   1

.globl	_jump_fcontext
.def	_jump_fcontext;	.scl	2;	.type	32;	.endef
_jump_fcontext:
    /* prepare stack */
    leal  -0x2c(%esp), %esp

#if !defined(BOOST_USE_TSX)
    /* save MMX control- and status-word */
    stmxcsr  (%esp)
    /* save x87 control-word */
    fnstcw  0x4(%esp)
#endif

    /* load NT_TIB */
    movl  %fs:(0x18), %edx
    /* load fiber local storage */
    movl  0x10(%edx), %eax
    movl  %eax, 0x8(%esp)
    /* load current dealloction stack */
    movl  0xe0c(%edx), %eax
    movl  %eax, 0xc(%esp)
    /* load current stack limit */
    movl  0x8(%edx), %eax
    movl  %eax, 0x10(%esp)
    /* load current stack base */
    movl  0x4(%edx), %eax
    movl  %eax, 0x14(%esp)
    /* load current SEH exception list */
    movl  (%edx), %eax
    movl  %eax, 0x18(%esp)

    movl  %edi, 0x1c(%esp)  /* save EDI */
    movl  %esi, 0x20(%esp)  /* save ESI */
    movl  %ebx, 0x24(%esp)  /* save EBX */
    movl  %ebp, 0x28(%esp)  /* save EBP */

    /* store ESP (pointing to context-data) in EAX */
    movl  %esp, %eax

    /* firstarg of jump_fcontext() == fcontext to jump to */
    movl  0x30(%esp), %ecx

    /* restore ESP (pointing to context-data) from ECX */
    movl  %ecx, %esp

#if !defined(BOOST_USE_TSX)
    /* restore MMX control- and status-word */
    ldmxcsr  (%esp)
    /* restore x87 control-word */
    fldcw  0x4(%esp)
#endif

    /* restore NT_TIB into EDX */
    movl  %fs:(0x18), %edx
    /* restore fiber local storage */
    movl  0x8(%esp), %ecx
    movl  %ecx, 0x10(%edx)
    /* restore current deallocation stack */
    movl  0xc(%esp), %ecx
    movl  %ecx, 0xe0c(%edx)
    /* restore current stack limit */
    movl  0x10(%esp), %ecx
    movl  %ecx, 0x8(%edx)
    /* restore current stack base */
    movl  0x14(%esp), %ecx
    movl  %ecx, 0x4(%edx)
    /* restore current SEH exception list */
    movl  0x18(%esp), %ecx
    movl  %ecx, (%edx)

    movl  0x2c(%esp), %ecx  /* restore EIP */

    movl  0x1c(%esp), %edi  /* restore EDI */
    movl  0x20(%esp), %esi  /* restore ESI */
    movl  0x24(%esp), %ebx  /* restore EBX */
    movl  0x28(%esp), %ebp  /* restore EBP */

    /* prepare stack */
    leal  0x30(%esp), %esp

    /* return transfer_t */
    /* FCTX == EAX, DATA == EDX */
    movl  0x34(%eax), %edx

    /* jump to context */
    jmp *%ecx

.section .drectve
.ascii " -export:\"jump_fcontext\""
