.global DW_CFA_expression_testcase

.extern recover_register

.text

# CFI expressions were added in DWARF v3 to allow compilers to specify memory
# locations or register values using DWARF programs. These programs are simple
# stack-based operations which allow the compiler to encode integer mathematics
# and other complex logic. CFI expressions are therefore more powerful than the
# conventional register + offset schemes.
#
# These tests capture a bug we have fixed in libunwind. CFI expression programs
# always start with the current CFA pushed onto the stack. This file contains a
# pair of routines which test CFI expression parsing. Specifically they test
# DW_CFA_expression logic, which uses DWARF expressions to compute the address
# where a non-volatile register was stored.
#
# Main calls DW_CFA_expression_testcase, which sets up known state in a
# non-volatile (caller-saved) register. We use r12 for this purpose. After this
# DW_CFA_expression_testcase then calls DW_CFA_expression_inner, which clobbers
# r12 after stashing its value on the stack. This routine contains a DWARF3 CFI
# expression to restore the value of r12 on unwind which should allow libunwind
# to recover clobbered state. DW_CFA_expression_inner calls recover_register to
# retrieve the cached register value. This function recovers the register value
# by using libunwind to unwind the stack through DW_CFA_expression_inner and up
# to the call site in DW_CFA_expression_testcase. If our expression is correct,
# libunwind will be able to restore r12 from the stack.
#
# BE CAREFUL WITH rdi, rsi, rax HERE! The arguments to recover_register are
# passed in via rdi, rsi and I just let them flow through unchanged. Similarly
# RAX flows back unchanged. Adding any function calls to the below may clobber
# these registers and cause this test to fail mysteriously.


########################################################
# Test: Restoring a register using a DW_CFA_expression #
# which uses implicit CFA pushed onto stack.           #
########################################################

.type DW_CFA_expression_testcase STT_FUNC
DW_CFA_expression_testcase:
  .cfi_startproc
  push %r12
  .cfi_adjust_cfa_offset 8
  # Move our sentinel (known) value into non-volatile (Callee-saved) r12
  mov $111222333, %r12
  .cfi_rel_offset %r12, 0
  call DW_CFA_expression_inner
  pop %r12
  .cfi_restore %r12
  .cfi_adjust_cfa_offset -8
  ret
  .cfi_endproc
.size DW_CFA_expression_testcase,.-DW_CFA_expression_testcase

.type DW_CFA_expression_inner STT_FUNC
DW_CFA_expression_inner:
  .cfi_startproc
  push %r12
  .cfi_adjust_cfa_offset 8
  # !! IMPORTANT BIT !! The test is all about how we parse the following bytes.
  # Now we use an expression to describe where our sentinel value is stored:
  # DW_CFA_expression(0x10), r12(0x0c), Length(0x02),        (preamble)
  # DW_OP_lit16(0x40), DW_OP_minus(0x1c)                     (instructions)
  # Parsing starts with the CFA on the stack, then pushes 16, then does a minus
  # which is eqivalent to a=pop(), b=pop(), push(b-a), leaving us with a value
  # of cfa-16 (cfa points at old rsp, cfa-8 is our rip, so we stored r12 at
  # cfa-16).
  xor %r12, %r12                             # Trash r12
  .cfi_escape 0x10, 0x0c, 0x2, 0x40, 0x1c   # DW_CFA_expression for recovery
  call recover_register
  pop %r12
  .cfi_restore %r12
  .cfi_adjust_cfa_offset -8
  ret
  .cfi_endproc
.size DW_CFA_expression_inner,.-DW_CFA_expression_inner
