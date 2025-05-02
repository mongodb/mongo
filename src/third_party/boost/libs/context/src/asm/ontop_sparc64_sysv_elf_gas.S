/*
       Copyright Claudio Jeker 2024
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/*
 * transfer_t ontop_fcontext(fcontext_t const to, void *vp, transfer_t (*fn)(transfer_t));
 */
#define CC64FSZ 176
#define BIAS 2047
#define SP 128
#define I7 136

.file "ontop_sparc64_sysv_elf_gas.S"
.text
.align  4
.global ontop_fcontext
.type   ontop_fcontext, %function
ontop_fcontext:
	# prepare stack
	save	%sp, -CC64FSZ, %sp

	# store framepointer and return address in slots reserved
	# for arguments
	stx %fp, [%sp + BIAS + SP]
	stx %i7, [%sp + BIAS + I7]
	mov %sp, %o0
	# force flush register windows to stack and with that save context
	flushw
	# get SP (pointing to new context-data) from %i0 param
	mov %i0, %sp
	# load framepointer and return address from context
	ldx [%sp + BIAS + SP], %fp
	ldx [%sp + BIAS + I7], %i7

	# ontop_fcontext requires to directly call a function on top of the
	# current frame so restore register window before doing the jump
	# to the context function which then is in %o2. Do not clobber
	# %o7 in the jump so that (*fn)() returns to that address.
	restore %o0, %g0, %o0
	# restore old %sp (pointing to old context-data) in %o0
	# *data stored in %o1 was not modified

	jmpl %o2, %g0
	 nop
.size	jump_fcontext,.-jump_fcontext
# Mark that we don't need executable stack.
.section .note.GNU-stack,"",%progbits
