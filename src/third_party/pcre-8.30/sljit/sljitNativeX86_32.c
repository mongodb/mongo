/*
 *    Stack-less Just-In-Time compiler
 *
 *    Copyright 2009-2012 Zoltan Herczeg (hzmester@freemail.hu). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this list of
 *      conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice, this list
 *      of conditions and the following disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDER(S) OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* x86 32-bit arch dependent functions. */

static int emit_do_imm(struct sljit_compiler *compiler, sljit_ub opcode, sljit_w imm)
{
	sljit_ub *buf;

	buf = (sljit_ub*)ensure_buf(compiler, 1 + 1 + sizeof(sljit_w));
	FAIL_IF(!buf);
	INC_SIZE(1 + sizeof(sljit_w));
	*buf++ = opcode;
	*(sljit_w*)buf = imm;
	return SLJIT_SUCCESS;
}

static sljit_ub* generate_far_jump_code(struct sljit_jump *jump, sljit_ub *code_ptr, int type)
{
	if (type == SLJIT_JUMP) {
		*code_ptr++ = 0xe9;
		jump->addr++;
	}
	else if (type >= SLJIT_FAST_CALL) {
		*code_ptr++ = 0xe8;
		jump->addr++;
	}
	else {
		*code_ptr++ = 0x0f;
		*code_ptr++ = get_jump_code(type);
		jump->addr += 2;
	}

	if (jump->flags & JUMP_LABEL)
		jump->flags |= PATCH_MW;
	else
		*(sljit_w*)code_ptr = jump->u.target - (jump->addr + 4);
	code_ptr += 4;

	return code_ptr;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_enter(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	int size;
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_enter(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	compiler->args = args;
	compiler->flags_saved = 0;

#if (defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	size = 1 + (saveds <= 3 ? saveds : 3) + (args > 0 ? (args * 2) : 0) + (args > 2 ? 2 : 0);
#else
	size = 1 + (saveds <= 3 ? saveds : 3) + (args > 0 ? (2 + args * 3) : 0);
#endif
	buf = (sljit_ub*)ensure_buf(compiler, 1 + size);
	FAIL_IF(!buf);

	INC_SIZE(size);
	PUSH_REG(reg_map[TMP_REGISTER]);
#if !(defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	if (args > 0) {
		*buf++ = 0x8b;
		*buf++ = 0xc4 | (reg_map[TMP_REGISTER] << 3);
	}
#endif
	if (saveds > 2)
		PUSH_REG(reg_map[SLJIT_SAVED_REG3]);
	if (saveds > 1)
		PUSH_REG(reg_map[SLJIT_SAVED_REG2]);
	if (saveds > 0)
		PUSH_REG(reg_map[SLJIT_SAVED_REG1]);

#if (defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	if (args > 0) {
		*buf++ = 0x8b;
		*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG1] << 3) | reg_map[SLJIT_TEMPORARY_REG3];
	}
	if (args > 1) {
		*buf++ = 0x8b;
		*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG2] << 3) | reg_map[SLJIT_TEMPORARY_REG2];
	}
	if (args > 2) {
		*buf++ = 0x8b;
		*buf++ = 0x44 | (reg_map[SLJIT_SAVED_REG3] << 3);
		*buf++ = 0x24;
		*buf++ = sizeof(sljit_w) * (3 + 2); /* saveds >= 3 as well. */
	}
#else
	if (args > 0) {
		*buf++ = 0x8b;
		*buf++ = 0x40 | (reg_map[SLJIT_SAVED_REG1] << 3) | reg_map[TMP_REGISTER];
		*buf++ = sizeof(sljit_w) * 2;
	}
	if (args > 1) {
		*buf++ = 0x8b;
		*buf++ = 0x40 | (reg_map[SLJIT_SAVED_REG2] << 3) | reg_map[TMP_REGISTER];
		*buf++ = sizeof(sljit_w) * 3;
	}
	if (args > 2) {
		*buf++ = 0x8b;
		*buf++ = 0x40 | (reg_map[SLJIT_SAVED_REG3] << 3) | reg_map[TMP_REGISTER];
		*buf++ = sizeof(sljit_w) * 4;
	}
#endif

	local_size = (local_size + sizeof(sljit_uw) - 1) & ~(sizeof(sljit_uw) - 1);
	compiler->temporaries_start = local_size;
	if (temporaries > 3)
		local_size += (temporaries - 3) * sizeof(sljit_uw);
	compiler->saveds_start = local_size;
	if (saveds > 3)
		local_size += (saveds - 3) * sizeof(sljit_uw);

#ifdef _WIN32
	if (local_size > 1024) {
		FAIL_IF(emit_do_imm(compiler, 0xb8 + reg_map[SLJIT_TEMPORARY_REG1], local_size));
		FAIL_IF(sljit_emit_ijump(compiler, SLJIT_CALL1, SLJIT_IMM, SLJIT_FUNC_OFFSET(sljit_touch_stack)));
	}
#endif

	compiler->local_size = local_size;
	if (local_size > 0)
		return emit_non_cum_binary(compiler, 0x2b, 0x29, 0x5 << 3, 0x2d,
			SLJIT_LOCALS_REG, 0, SLJIT_LOCALS_REG, 0, SLJIT_IMM, local_size);

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_context(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	CHECK_ERROR_VOID();
	check_sljit_set_context(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	compiler->args = args;
	compiler->local_size = (local_size + sizeof(sljit_uw) - 1) & ~(sizeof(sljit_uw) - 1);
	compiler->temporaries_start = compiler->local_size;
	if (temporaries > 3)
		compiler->local_size += (temporaries - 3) * sizeof(sljit_uw);
	compiler->saveds_start = compiler->local_size;
	if (saveds > 3)
		compiler->local_size += (saveds - 3) * sizeof(sljit_uw);
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_return(struct sljit_compiler *compiler, int op, int src, sljit_w srcw)
{
	int size;
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_return(compiler, op, src, srcw);
	SLJIT_ASSERT(compiler->args >= 0);

	compiler->flags_saved = 0;
	FAIL_IF(emit_mov_before_return(compiler, op, src, srcw));

	if (compiler->local_size > 0)
		FAIL_IF(emit_cum_binary(compiler, 0x03, 0x01, 0x0 << 3, 0x05,
				SLJIT_LOCALS_REG, 0, SLJIT_LOCALS_REG, 0, SLJIT_IMM, compiler->local_size));

	size = 2 + (compiler->saveds <= 3 ? compiler->saveds : 3);
#if (defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	if (compiler->args > 2)
		size += 2;
#else
	if (compiler->args > 0)
		size += 2;
#endif
	buf = (sljit_ub*)ensure_buf(compiler, 1 + size);
	FAIL_IF(!buf);

	INC_SIZE(size);

	if (compiler->saveds > 0)
		POP_REG(reg_map[SLJIT_SAVED_REG1]);
	if (compiler->saveds > 1)
		POP_REG(reg_map[SLJIT_SAVED_REG2]);
	if (compiler->saveds > 2)
		POP_REG(reg_map[SLJIT_SAVED_REG3]);
	POP_REG(reg_map[TMP_REGISTER]);
#if (defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	if (compiler->args > 2)
		RETN(sizeof(sljit_w));
	else
		RET();
#else
	if (compiler->args > 0)
		RETN(compiler->args * sizeof(sljit_w));
	else
		RET();
#endif

	return SLJIT_SUCCESS;
}

/* --------------------------------------------------------------------- */
/*  Operators                                                            */
/* --------------------------------------------------------------------- */

/* Size contains the flags as well. */
static sljit_ub* emit_x86_instruction(struct sljit_compiler *compiler, int size,
	/* The register or immediate operand. */
	int a, sljit_w imma,
	/* The general operand (not immediate). */
	int b, sljit_w immb)
{
	sljit_ub *buf;
	sljit_ub *buf_ptr;
	int flags = size & ~0xf;
	int inst_size;

	/* Both cannot be switched on. */
	SLJIT_ASSERT((flags & (EX86_BIN_INS | EX86_SHIFT_INS)) != (EX86_BIN_INS | EX86_SHIFT_INS));
	/* Size flags not allowed for typed instructions. */
	SLJIT_ASSERT(!(flags & (EX86_BIN_INS | EX86_SHIFT_INS)) || (flags & (EX86_BYTE_ARG | EX86_HALF_ARG)) == 0);
	/* Both size flags cannot be switched on. */
	SLJIT_ASSERT((flags & (EX86_BYTE_ARG | EX86_HALF_ARG)) != (EX86_BYTE_ARG | EX86_HALF_ARG));
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
	/* SSE2 and immediate is not possible. */
	SLJIT_ASSERT(!(a & SLJIT_IMM) || !(flags & EX86_SSE2));
#endif

	size &= 0xf;
	inst_size = size;

#if (defined SLJIT_SSE2 && SLJIT_SSE2)
	if (flags & EX86_PREF_F2)
		inst_size++;
#endif
	if (flags & EX86_PREF_66)
		inst_size++;

	/* Calculate size of b. */
	inst_size += 1; /* mod r/m byte. */
	if (b & SLJIT_MEM) {
		if ((b & 0x0f) == SLJIT_UNUSED)
			inst_size += sizeof(sljit_w);
		else if (immb != 0 && !(b & 0xf0)) {
			/* Immediate operand. */
			if (immb <= 127 && immb >= -128)
				inst_size += sizeof(sljit_b);
			else
				inst_size += sizeof(sljit_w);
		}

		if ((b & 0xf) == SLJIT_LOCALS_REG && !(b & 0xf0))
			b |= SLJIT_LOCALS_REG << 4;

		if ((b & 0xf0) != SLJIT_UNUSED)
			inst_size += 1; /* SIB byte. */
	}

	/* Calculate size of a. */
	if (a & SLJIT_IMM) {
		if (flags & EX86_BIN_INS) {
			if (imma <= 127 && imma >= -128) {
				inst_size += 1;
				flags |= EX86_BYTE_ARG;
			} else
				inst_size += 4;
		}
		else if (flags & EX86_SHIFT_INS) {
			imma &= 0x1f;
			if (imma != 1) {
				inst_size ++;
				flags |= EX86_BYTE_ARG;
			}
		} else if (flags & EX86_BYTE_ARG)
			inst_size++;
		else if (flags & EX86_HALF_ARG)
			inst_size += sizeof(short);
		else
			inst_size += sizeof(sljit_w);
	}
	else
		SLJIT_ASSERT(!(flags & EX86_SHIFT_INS) || a == SLJIT_PREF_SHIFT_REG);

	buf = (sljit_ub*)ensure_buf(compiler, 1 + inst_size);
	PTR_FAIL_IF(!buf);

	/* Encoding the byte. */
	INC_SIZE(inst_size);
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
	if (flags & EX86_PREF_F2)
		*buf++ = 0xf2;
#endif
	if (flags & EX86_PREF_66)
		*buf++ = 0x66;

	buf_ptr = buf + size;

	/* Encode mod/rm byte. */
	if (!(flags & EX86_SHIFT_INS)) {
		if ((flags & EX86_BIN_INS) && (a & SLJIT_IMM))
			*buf = (flags & EX86_BYTE_ARG) ? 0x83 : 0x81;

		if ((a & SLJIT_IMM) || (a == 0))
			*buf_ptr = 0;
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
		else if (!(flags & EX86_SSE2))
			*buf_ptr = reg_map[a] << 3;
		else
			*buf_ptr = a << 3;
#else
		else
			*buf_ptr = reg_map[a] << 3;
#endif
	}
	else {
		if (a & SLJIT_IMM) {
			if (imma == 1)
				*buf = 0xd1;
			else
				*buf = 0xc1;
		} else
			*buf = 0xd3;
		*buf_ptr = 0;
	}

	if (!(b & SLJIT_MEM))
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
		*buf_ptr++ |= 0xc0 + ((!(flags & EX86_SSE2)) ? reg_map[b] : b);
#else
		*buf_ptr++ |= 0xc0 + reg_map[b];
#endif
	else if ((b & 0x0f) != SLJIT_UNUSED) {
		if ((b & 0xf0) == SLJIT_UNUSED || (b & 0xf0) == (SLJIT_LOCALS_REG << 4)) {
			if (immb != 0) {
				if (immb <= 127 && immb >= -128)
					*buf_ptr |= 0x40;
				else
					*buf_ptr |= 0x80;
			}

			if ((b & 0xf0) == SLJIT_UNUSED)
				*buf_ptr++ |= reg_map[b & 0x0f];
			else {
				*buf_ptr++ |= 0x04;
				*buf_ptr++ = reg_map[b & 0x0f] | (reg_map[(b >> 4) & 0x0f] << 3);
			}

			if (immb != 0) {
				if (immb <= 127 && immb >= -128)
					*buf_ptr++ = immb; /* 8 bit displacement. */
				else {
					*(sljit_w*)buf_ptr = immb; /* 32 bit displacement. */
					buf_ptr += sizeof(sljit_w);
				}
			}
		}
		else {
			*buf_ptr++ |= 0x04;
			*buf_ptr++ = reg_map[b & 0x0f] | (reg_map[(b >> 4) & 0x0f] << 3) | (immb << 6);
		}
	}
	else {
		*buf_ptr++ |= 0x05;
		*(sljit_w*)buf_ptr = immb; /* 32 bit displacement. */
		buf_ptr += sizeof(sljit_w);
	}

	if (a & SLJIT_IMM) {
		if (flags & EX86_BYTE_ARG)
			*buf_ptr = imma;
		else if (flags & EX86_HALF_ARG)
			*(short*)buf_ptr = imma;
		else if (!(flags & EX86_SHIFT_INS))
			*(sljit_w*)buf_ptr = imma;
	}

	return !(flags & EX86_SHIFT_INS) ? buf : (buf + 1);
}

/* --------------------------------------------------------------------- */
/*  Call / return instructions                                           */
/* --------------------------------------------------------------------- */

static SLJIT_INLINE int call_with_args(struct sljit_compiler *compiler, int type)
{
	sljit_ub *buf;

#if (defined SLJIT_X86_32_FASTCALL && SLJIT_X86_32_FASTCALL)
	buf = (sljit_ub*)ensure_buf(compiler, type >= SLJIT_CALL3 ? 1 + 2 + 1 : 1 + 2);
	FAIL_IF(!buf);
	INC_SIZE(type >= SLJIT_CALL3 ? 2 + 1 : 2);

	if (type >= SLJIT_CALL3)
		PUSH_REG(reg_map[SLJIT_TEMPORARY_REG3]);
	*buf++ = 0x8b;
	*buf++ = 0xc0 | (reg_map[SLJIT_TEMPORARY_REG3] << 3) | reg_map[SLJIT_TEMPORARY_REG1];
#else
	buf = (sljit_ub*)ensure_buf(compiler, type - SLJIT_CALL0 + 1);
	FAIL_IF(!buf);
	INC_SIZE(type - SLJIT_CALL0);
	if (type >= SLJIT_CALL3)
		PUSH_REG(reg_map[SLJIT_TEMPORARY_REG3]);
	if (type >= SLJIT_CALL2)
		PUSH_REG(reg_map[SLJIT_TEMPORARY_REG2]);
	PUSH_REG(reg_map[SLJIT_TEMPORARY_REG1]);
#endif
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_enter(struct sljit_compiler *compiler, int dst, sljit_w dstw, int args, int temporaries, int saveds, int local_size)
{
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_fast_enter(compiler, dst, dstw, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	compiler->args = args;
	compiler->local_size = (local_size + sizeof(sljit_uw) - 1) & ~(sizeof(sljit_uw) - 1);
	compiler->temporaries_start = compiler->local_size;
	if (temporaries > 3)
		compiler->local_size += (temporaries - 3) * sizeof(sljit_uw);
	compiler->saveds_start = compiler->local_size;
	if (saveds > 3)
		compiler->local_size += (saveds - 3) * sizeof(sljit_uw);

	CHECK_EXTRA_REGS(dst, dstw, (void)0);

	if (dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_NO_REGISTERS) {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 1);
		FAIL_IF(!buf);

		INC_SIZE(1);
		POP_REG(reg_map[dst]);
		return SLJIT_SUCCESS;
	}
	else if (dst & SLJIT_MEM) {
		buf = emit_x86_instruction(compiler, 1, 0, 0, dst, dstw);
		FAIL_IF(!buf);
		*buf++ = 0x8f;
		return SLJIT_SUCCESS;
	}

	/* For UNUSED dst. Uncommon, but possible. */
	buf = (sljit_ub*)ensure_buf(compiler, 1 + 1);
	FAIL_IF(!buf);

	INC_SIZE(1);
	POP_REG(reg_map[TMP_REGISTER]);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_return(struct sljit_compiler *compiler, int src, sljit_w srcw)
{
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_fast_return(compiler, src, srcw);

	CHECK_EXTRA_REGS(src, srcw, (void)0);

	if (src >= SLJIT_TEMPORARY_REG1 && src <= SLJIT_NO_REGISTERS) {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 1 + 1);
		FAIL_IF(!buf);

		INC_SIZE(1 + 1);
		PUSH_REG(reg_map[src]);
	}
	else if (src & SLJIT_MEM) {
		buf = emit_x86_instruction(compiler, 1, 0, 0, src, srcw);
		FAIL_IF(!buf);
		*buf++ = 0xff;
		*buf |= 6 << 3;

		buf = (sljit_ub*)ensure_buf(compiler, 1 + 1);
		FAIL_IF(!buf);
		INC_SIZE(1);
	}
	else {
		/* SLJIT_IMM. */
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 5 + 1);
		FAIL_IF(!buf);

		INC_SIZE(5 + 1);
		*buf++ = 0x68;
		*(sljit_w*)buf = srcw;
		buf += sizeof(sljit_w);
	}

	RET();
	return SLJIT_SUCCESS;
}
