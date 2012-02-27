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

/* x86 64-bit arch dependent functions. */

static int emit_load_imm64(struct sljit_compiler *compiler, int reg, sljit_w imm)
{
	sljit_ub *buf;

	buf = (sljit_ub*)ensure_buf(compiler, 1 + 2 + sizeof(sljit_w));
	FAIL_IF(!buf);
	INC_SIZE(2 + sizeof(sljit_w));
	*buf++ = REX_W | ((reg_map[reg] <= 7) ? 0 : REX_B);
	*buf++ = 0xb8 + (reg_map[reg] & 0x7);
	*(sljit_w*)buf = imm;
	return SLJIT_SUCCESS;
}

static sljit_ub* generate_far_jump_code(struct sljit_jump *jump, sljit_ub *code_ptr, int type)
{
	if (type < SLJIT_JUMP) {
		*code_ptr++ = get_jump_code(type ^ 0x1) - 0x10;
		*code_ptr++ = 10 + 3;
	}

	SLJIT_COMPILE_ASSERT(reg_map[TMP_REG3] == 9, tmp3_is_9_first);
	*code_ptr++ = REX_W | REX_B;
	*code_ptr++ = 0xb8 + 1;
	jump->addr = (sljit_uw)code_ptr;

	if (jump->flags & JUMP_LABEL)
		jump->flags |= PATCH_MD;
	else
		*(sljit_w*)code_ptr = jump->u.target;

	code_ptr += sizeof(sljit_w);
	*code_ptr++ = REX_B;
	*code_ptr++ = 0xff;
	*code_ptr++ = (type >= SLJIT_FAST_CALL) ? 0xd1 /* call */ : 0xe1 /* jmp */;

	return code_ptr;
}

static sljit_ub* generate_fixed_jump(sljit_ub *code_ptr, sljit_w addr, int type)
{
	sljit_w delta = addr - ((sljit_w)code_ptr + 1 + sizeof(sljit_hw));

	if (delta <= SLJIT_W(0x7fffffff) && delta >= SLJIT_W(-0x80000000)) {
		*code_ptr++ = (type == 2) ? 0xe8 /* call */ : 0xe9 /* jmp */;
		*(sljit_w*)code_ptr = delta;
	}
	else {
		SLJIT_COMPILE_ASSERT(reg_map[TMP_REG3] == 9, tmp3_is_9_second);
		*code_ptr++ = REX_W | REX_B;
		*code_ptr++ = 0xb8 + 1;
		*(sljit_w*)code_ptr = addr;
		code_ptr += sizeof(sljit_w);
		*code_ptr++ = REX_B;
		*code_ptr++ = 0xff;
		*code_ptr++ = (type == 2) ? 0xd1 /* call */ : 0xe1 /* jmp */;
	}

	return code_ptr;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_enter(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	int size, pushed_size;
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_enter(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	compiler->flags_saved = 0;

	size = saveds;
	/* Including the return address saved by the call instruction. */
	pushed_size = (saveds + 1) * sizeof(sljit_w);
#ifndef _WIN64
	if (saveds >= 2)
		size += saveds - 1;
#else
	/* Saving the virtual stack pointer. */
	compiler->has_locals = local_size > 0;
	if (local_size > 0) {
		size += 2;
		pushed_size += sizeof(sljit_w);
	}
	if (saveds >= 4)
		size += saveds - 3;
	if (temporaries >= 5) {
		size += (5 - 4) * 2;
		pushed_size += sizeof(sljit_w);
	}
#endif
	size += args * 3;
	if (size > 0) {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + size);
		FAIL_IF(!buf);

		INC_SIZE(size);
		if (saveds >= 5) {
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_EREG2] >= 8, saved_ereg2_is_hireg);
			*buf++ = REX_B;
			PUSH_REG(reg_lmap[SLJIT_SAVED_EREG2]);
		}
		if (saveds >= 4) {
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_EREG1] >= 8, saved_ereg1_is_hireg);
			*buf++ = REX_B;
			PUSH_REG(reg_lmap[SLJIT_SAVED_EREG1]);
		}
		if (saveds >= 3) {
#ifndef _WIN64
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_REG3] >= 8, saved_reg3_is_hireg);
			*buf++ = REX_B;
#else
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_REG3] < 8, saved_reg3_is_loreg);
#endif
			PUSH_REG(reg_lmap[SLJIT_SAVED_REG3]);
		}
		if (saveds >= 2) {
#ifndef _WIN64
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_REG2] >= 8, saved_reg2_is_hireg);
			*buf++ = REX_B;
#else
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_REG2] < 8, saved_reg2_is_loreg);
#endif
			PUSH_REG(reg_lmap[SLJIT_SAVED_REG2]);
		}
		if (saveds >= 1) {
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_SAVED_REG1] < 8, saved_reg1_is_loreg);
			PUSH_REG(reg_lmap[SLJIT_SAVED_REG1]);
		}
#ifdef _WIN64
		if (temporaries >= 5) {
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_TEMPORARY_EREG2] >= 8, temporary_ereg2_is_hireg);
			*buf++ = REX_B;
			PUSH_REG(reg_lmap[SLJIT_TEMPORARY_EREG2]);
		}
		if (local_size > 0) {
			SLJIT_COMPILE_ASSERT(reg_map[SLJIT_LOCALS_REG] >= 8, locals_reg_is_hireg);
			*buf++ = REX_B;
			PUSH_REG(reg_lmap[SLJIT_LOCALS_REG]);
		}
#endif

#ifndef _WIN64
		if (args > 0) {
			*buf++ = REX_W;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG1] << 3) | 0x7;
		}
		if (args > 1) {
			*buf++ = REX_W | REX_R;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_lmap[SLJIT_SAVED_REG2] << 3) | 0x6;
		}
		if (args > 2) {
			*buf++ = REX_W | REX_R;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_lmap[SLJIT_SAVED_REG3] << 3) | 0x2;
		}
#else
		if (args > 0) {
			*buf++ = REX_W;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG1] << 3) | 0x1;
		}
		if (args > 1) {
			*buf++ = REX_W;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG2] << 3) | 0x2;
		}
		if (args > 2) {
			*buf++ = REX_W | REX_B;
			*buf++ = 0x8b;
			*buf++ = 0xc0 | (reg_map[SLJIT_SAVED_REG3] << 3) | 0x0;
		}
#endif
	}

	local_size = ((local_size + pushed_size + 16 - 1) & ~(16 - 1)) - pushed_size;
#ifdef _WIN64
	local_size += 4 * sizeof(sljit_w);
	compiler->local_size = local_size;
	if (local_size > 1024) {
		/* Allocate the stack for the function itself. */
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 4);
		FAIL_IF(!buf);
		INC_SIZE(4);
		*buf++ = REX_W;
		*buf++ = 0x83;
		*buf++ = 0xc0 | (5 << 3) | 4;
		/* Pushed size must be divisible by 8. */
		SLJIT_ASSERT(!(pushed_size & 0x7));
		if (pushed_size & 0x8) {
			*buf++ = 5 * sizeof(sljit_w);
			local_size -= 5 * sizeof(sljit_w);
		} else {
			*buf++ = 4 * sizeof(sljit_w);
			local_size -= 4 * sizeof(sljit_w);
		}
		FAIL_IF(emit_load_imm64(compiler, SLJIT_TEMPORARY_REG1, local_size));
		FAIL_IF(sljit_emit_ijump(compiler, SLJIT_CALL1, SLJIT_IMM, SLJIT_FUNC_OFFSET(sljit_touch_stack)));
	}
#else
	compiler->local_size = local_size;
	if (local_size > 0) {
#endif
		/* In case of Win64, local_size is always > 4 * sizeof(sljit_w) */
		if (local_size <= 127) {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 4);
			FAIL_IF(!buf);
			INC_SIZE(4);
			*buf++ = REX_W;
			*buf++ = 0x83;
			*buf++ = 0xc0 | (5 << 3) | 4;
			*buf++ = local_size;
		}
		else {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 7);
			FAIL_IF(!buf);
			INC_SIZE(7);
			*buf++ = REX_W;
			*buf++ = 0x81;
			*buf++ = 0xc0 | (5 << 3) | 4;
			*(sljit_hw*)buf = local_size;
			buf += sizeof(sljit_hw);
		}
#ifndef _WIN64
	}
#endif

#ifdef _WIN64
	if (compiler->has_locals) {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 5);
		FAIL_IF(!buf);
		INC_SIZE(5);
		*buf++ = REX_W | REX_R;
		*buf++ = 0x8d;
		*buf++ = 0x40 | (reg_lmap[SLJIT_LOCALS_REG] << 3) | 0x4;
		*buf++ = 0x24;
		*buf = 4 * sizeof(sljit_w);
	}
#endif

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_context(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	int pushed_size;

	CHECK_ERROR_VOID();
	check_sljit_set_context(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	/* Including the return address saved by the call instruction. */
	pushed_size = (saveds + 1) * sizeof(sljit_w);
#ifdef _WIN64
	compiler->has_locals = local_size > 0;
	if (local_size > 0)
		pushed_size += sizeof(sljit_w);
	if (temporaries >= 5)
		pushed_size += sizeof(sljit_w);
#endif
	compiler->local_size = ((local_size + pushed_size + 16 - 1) & ~(16 - 1)) - pushed_size;
#ifdef _WIN64
	compiler->local_size += 4 * sizeof(sljit_w);
#endif
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_return(struct sljit_compiler *compiler, int op, int src, sljit_w srcw)
{
	int size;
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_return(compiler, op, src, srcw);

	compiler->flags_saved = 0;
	FAIL_IF(emit_mov_before_return(compiler, op, src, srcw));

	if (compiler->local_size > 0) {
		if (compiler->local_size <= 127) {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 4);
			FAIL_IF(!buf);
			INC_SIZE(4);
			*buf++ = REX_W;
			*buf++ = 0x83;
			*buf++ = 0xc0 | (0 << 3) | 4;
			*buf = compiler->local_size;
		}
		else {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 7);
			FAIL_IF(!buf);
			INC_SIZE(7);
			*buf++ = REX_W;
			*buf++ = 0x81;
			*buf++ = 0xc0 | (0 << 3) | 4;
			*(sljit_hw*)buf = compiler->local_size;
		}
	}

	size = 1 + compiler->saveds;
#ifndef _WIN64
	if (compiler->saveds >= 2)
		size += compiler->saveds - 1;
#else
	if (compiler->has_locals)
		size += 2;
	if (compiler->saveds >= 4)
		size += compiler->saveds - 3;
	if (compiler->temporaries >= 5)
		size += (5 - 4) * 2;
#endif
	buf = (sljit_ub*)ensure_buf(compiler, 1 + size);
	FAIL_IF(!buf);

	INC_SIZE(size);

#ifdef _WIN64
	if (compiler->has_locals) {
		*buf++ = REX_B;
		POP_REG(reg_lmap[SLJIT_LOCALS_REG]);
	}
	if (compiler->temporaries >= 5) {
		*buf++ = REX_B;
		POP_REG(reg_lmap[SLJIT_TEMPORARY_EREG2]);
	}
#endif
	if (compiler->saveds >= 1)
		POP_REG(reg_map[SLJIT_SAVED_REG1]);
	if (compiler->saveds >= 2) {
#ifndef _WIN64
		*buf++ = REX_B;
#endif
		POP_REG(reg_lmap[SLJIT_SAVED_REG2]);
	}
	if (compiler->saveds >= 3) {
#ifndef _WIN64
		*buf++ = REX_B;
#endif
		POP_REG(reg_lmap[SLJIT_SAVED_REG3]);
	}
	if (compiler->saveds >= 4) {
		*buf++ = REX_B;
		POP_REG(reg_lmap[SLJIT_SAVED_EREG1]);
	}
	if (compiler->saveds >= 5) {
		*buf++ = REX_B;
		POP_REG(reg_lmap[SLJIT_SAVED_EREG2]);
	}

	RET();
	return SLJIT_SUCCESS;
}

/* --------------------------------------------------------------------- */
/*  Operators                                                            */
/* --------------------------------------------------------------------- */

static int emit_do_imm32(struct sljit_compiler *compiler, sljit_ub rex, sljit_ub opcode, sljit_w imm)
{
	sljit_ub *buf;

	if (rex != 0) {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 2 + sizeof(sljit_hw));
		FAIL_IF(!buf);
		INC_SIZE(2 + sizeof(sljit_hw));
		*buf++ = rex;
		*buf++ = opcode;
		*(sljit_hw*)buf = imm;
	}
	else {
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 1 + sizeof(sljit_hw));
		FAIL_IF(!buf);
		INC_SIZE(1 + sizeof(sljit_hw));
		*buf++ = opcode;
		*(sljit_hw*)buf = imm;
	}
	return SLJIT_SUCCESS;
}

static sljit_ub* emit_x86_instruction(struct sljit_compiler *compiler, int size,
	/* The register or immediate operand. */
	int a, sljit_w imma,
	/* The general operand (not immediate). */
	int b, sljit_w immb)
{
	sljit_ub *buf;
	sljit_ub *buf_ptr;
	sljit_ub rex = 0;
	int flags = size & ~0xf;
	int inst_size;

	/* The immediate operand must be 32 bit. */
	SLJIT_ASSERT(!(a & SLJIT_IMM) || compiler->mode32 || IS_HALFWORD(imma));
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

	if ((b & SLJIT_MEM) && !(b & 0xf0) && NOT_HALFWORD(immb)) {
		if (emit_load_imm64(compiler, TMP_REG3, immb))
			return NULL;
		immb = 0;
		if (b & 0xf)
			b |= TMP_REG3 << 4;
		else
			b |= TMP_REG3;
	}

	if (!compiler->mode32 && !(flags & EX86_NO_REXW))
		rex |= REX_W;
	else if (flags & EX86_REX)
		rex |= REX;

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
			inst_size += 1 + sizeof(sljit_hw); /* SIB byte required to avoid RIP based addressing. */
		else {
			if (reg_map[b & 0x0f] >= 8)
				rex |= REX_B;
			if (immb != 0 && !(b & 0xf0)) {
				/* Immediate operand. */
				if (immb <= 127 && immb >= -128)
					inst_size += sizeof(sljit_b);
				else
					inst_size += sizeof(sljit_hw);
			}
		}

#ifndef _WIN64
		if ((b & 0xf) == SLJIT_LOCALS_REG && (b & 0xf0) == 0)
			b |= SLJIT_LOCALS_REG << 4;
#endif

		if ((b & 0xf0) != SLJIT_UNUSED) {
			inst_size += 1; /* SIB byte. */
			if (reg_map[(b >> 4) & 0x0f] >= 8)
				rex |= REX_X;
		}
	}
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
	else if (!(flags & EX86_SSE2) && reg_map[b] >= 8)
		rex |= REX_B;
#else
	else if (reg_map[b] >= 8)
		rex |= REX_B;
#endif

	if (a & SLJIT_IMM) {
		if (flags & EX86_BIN_INS) {
			if (imma <= 127 && imma >= -128) {
				inst_size += 1;
				flags |= EX86_BYTE_ARG;
			} else
				inst_size += 4;
		}
		else if (flags & EX86_SHIFT_INS) {
			imma &= compiler->mode32 ? 0x1f : 0x3f;
			if (imma != 1) {
				inst_size ++;
				flags |= EX86_BYTE_ARG;
			}
		} else if (flags & EX86_BYTE_ARG)
			inst_size++;
		else if (flags & EX86_HALF_ARG)
			inst_size += sizeof(short);
		else
			inst_size += sizeof(sljit_hw);
	}
	else {
		SLJIT_ASSERT(!(flags & EX86_SHIFT_INS) || a == SLJIT_PREF_SHIFT_REG);
		/* reg_map[SLJIT_PREF_SHIFT_REG] is less than 8. */
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
		if (!(flags & EX86_SSE2) && reg_map[a] >= 8)
			rex |= REX_R;
#else
		if (reg_map[a] >= 8)
			rex |= REX_R;
#endif
	}

	if (rex)
		inst_size++;

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
	if (rex)
		*buf++ = rex;
	buf_ptr = buf + size;

	/* Encode mod/rm byte. */
	if (!(flags & EX86_SHIFT_INS)) {
		if ((flags & EX86_BIN_INS) && (a & SLJIT_IMM))
			*buf = (flags & EX86_BYTE_ARG) ? 0x83 : 0x81;

		if ((a & SLJIT_IMM) || (a == 0))
			*buf_ptr = 0;
#if (defined SLJIT_SSE2 && SLJIT_SSE2)
		else if (!(flags & EX86_SSE2))
			*buf_ptr = reg_lmap[a] << 3;
		else
			*buf_ptr = a << 3;
#else
		else
			*buf_ptr = reg_lmap[a] << 3;
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
		*buf_ptr++ |= 0xc0 + ((!(flags & EX86_SSE2)) ? reg_lmap[b] : b);
#else
		*buf_ptr++ |= 0xc0 + reg_lmap[b];
#endif
	else if ((b & 0x0f) != SLJIT_UNUSED) {
#ifdef _WIN64
		SLJIT_ASSERT((b & 0xf0) != (SLJIT_LOCALS_REG << 4));
#endif
		if ((b & 0xf0) == SLJIT_UNUSED || (b & 0xf0) == (SLJIT_LOCALS_REG << 4)) {
			if (immb != 0) {
				if (immb <= 127 && immb >= -128)
					*buf_ptr |= 0x40;
				else
					*buf_ptr |= 0x80;
			}

			if ((b & 0xf0) == SLJIT_UNUSED)
				*buf_ptr++ |= reg_lmap[b & 0x0f];
			else {
				*buf_ptr++ |= 0x04;
				*buf_ptr++ = reg_lmap[b & 0x0f] | (reg_lmap[(b >> 4) & 0x0f] << 3);
			}

			if (immb != 0) {
				if (immb <= 127 && immb >= -128)
					*buf_ptr++ = immb; /* 8 bit displacement. */
				else {
					*(sljit_hw*)buf_ptr = immb; /* 32 bit displacement. */
					buf_ptr += sizeof(sljit_hw);
				}
			}
		}
		else {
			*buf_ptr++ |= 0x04;
			*buf_ptr++ = reg_lmap[b & 0x0f] | (reg_lmap[(b >> 4) & 0x0f] << 3) | (immb << 6);
		}
	}
	else {
		*buf_ptr++ |= 0x04;
		*buf_ptr++ = 0x25;
		*(sljit_hw*)buf_ptr = immb; /* 32 bit displacement. */
		buf_ptr += sizeof(sljit_hw);
	}

	if (a & SLJIT_IMM) {
		if (flags & EX86_BYTE_ARG)
			*buf_ptr = imma;
		else if (flags & EX86_HALF_ARG)
			*(short*)buf_ptr = imma;
		else if (!(flags & EX86_SHIFT_INS))
			*(sljit_hw*)buf_ptr = imma;
	}

	return !(flags & EX86_SHIFT_INS) ? buf : (buf + 1);
}

/* --------------------------------------------------------------------- */
/*  Call / return instructions                                           */
/* --------------------------------------------------------------------- */

static SLJIT_INLINE int call_with_args(struct sljit_compiler *compiler, int type)
{
	sljit_ub *buf;

#ifndef _WIN64
	SLJIT_COMPILE_ASSERT(reg_map[SLJIT_TEMPORARY_REG2] == 6 && reg_map[SLJIT_TEMPORARY_REG1] < 8 && reg_map[SLJIT_TEMPORARY_REG3] < 8, args_registers);

	buf = (sljit_ub*)ensure_buf(compiler, 1 + ((type < SLJIT_CALL3) ? 3 : 6));
	FAIL_IF(!buf);
	INC_SIZE((type < SLJIT_CALL3) ? 3 : 6);
	if (type >= SLJIT_CALL3) {
		*buf++ = REX_W;
		*buf++ = 0x8b;
		*buf++ = 0xc0 | (0x2 << 3) | reg_lmap[SLJIT_TEMPORARY_REG3];
	}
	*buf++ = REX_W;
	*buf++ = 0x8b;
	*buf++ = 0xc0 | (0x7 << 3) | reg_lmap[SLJIT_TEMPORARY_REG1];
#else
	SLJIT_COMPILE_ASSERT(reg_map[SLJIT_TEMPORARY_REG2] == 2 && reg_map[SLJIT_TEMPORARY_REG1] < 8 && reg_map[SLJIT_TEMPORARY_REG3] < 8, args_registers);

	buf = (sljit_ub*)ensure_buf(compiler, 1 + ((type < SLJIT_CALL3) ? 3 : 6));
	FAIL_IF(!buf);
	INC_SIZE((type < SLJIT_CALL3) ? 3 : 6);
	if (type >= SLJIT_CALL3) {
		*buf++ = REX_W | REX_R;
		*buf++ = 0x8b;
		*buf++ = 0xc0 | (0x0 << 3) | reg_lmap[SLJIT_TEMPORARY_REG3];
	}
	*buf++ = REX_W;
	*buf++ = 0x8b;
	*buf++ = 0xc0 | (0x1 << 3) | reg_lmap[SLJIT_TEMPORARY_REG1];
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
	compiler->local_size = (local_size + sizeof(sljit_uw) - 1) & ~(sizeof(sljit_uw) - 1);
#ifdef _WIN64
	compiler->local_size += 4 * sizeof(sljit_w);
#endif

	/* For UNUSED dst. Uncommon, but possible. */
	if (dst == SLJIT_UNUSED)
		dst = TMP_REGISTER;

	if (dst >= SLJIT_TEMPORARY_REG1 && dst <= TMP_REGISTER) {
		if (reg_map[dst] < 8) {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 1);
			FAIL_IF(!buf);

			INC_SIZE(1);
			POP_REG(reg_lmap[dst]);
		}
		else {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 2);
			FAIL_IF(!buf);

			INC_SIZE(2);
			*buf++ = REX_B;
			POP_REG(reg_lmap[dst]);
		}
	}
	else if (dst & SLJIT_MEM) {
#if (defined SLJIT_CONFIG_X86_64 && SLJIT_CONFIG_X86_64)
		/* REX_W is not necessary (src is not immediate). */
		compiler->mode32 = 1;
#endif
		buf = emit_x86_instruction(compiler, 1, 0, 0, dst, dstw);
		FAIL_IF(!buf);
		*buf++ = 0x8f;
	}
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_return(struct sljit_compiler *compiler, int src, sljit_w srcw)
{
	sljit_ub *buf;

	CHECK_ERROR();
	check_sljit_emit_fast_return(compiler, src, srcw);

	CHECK_EXTRA_REGS(src, srcw, (void)0);

	if ((src & SLJIT_IMM) && NOT_HALFWORD(srcw)) {
		FAIL_IF(emit_load_imm64(compiler, TMP_REGISTER, srcw));
		src = TMP_REGISTER;
	}

	if (src >= SLJIT_TEMPORARY_REG1 && src <= TMP_REGISTER) {
		if (reg_map[src] < 8) {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 1 + 1);
			FAIL_IF(!buf);

			INC_SIZE(1 + 1);
			PUSH_REG(reg_lmap[src]);
		}
		else {
			buf = (sljit_ub*)ensure_buf(compiler, 1 + 2 + 1);
			FAIL_IF(!buf);

			INC_SIZE(2 + 1);
			*buf++ = REX_B;
			PUSH_REG(reg_lmap[src]);
		}
	}
	else if (src & SLJIT_MEM) {
#if (defined SLJIT_CONFIG_X86_64 && SLJIT_CONFIG_X86_64)
		/* REX_W is not necessary (src is not immediate). */
		compiler->mode32 = 1;
#endif
		buf = emit_x86_instruction(compiler, 1, 0, 0, src, srcw);
		FAIL_IF(!buf);
		*buf++ = 0xff;
		*buf |= 6 << 3;

		buf = (sljit_ub*)ensure_buf(compiler, 1 + 1);
		FAIL_IF(!buf);
		INC_SIZE(1);
	}
	else {
		SLJIT_ASSERT(IS_HALFWORD(srcw));
		/* SLJIT_IMM. */
		buf = (sljit_ub*)ensure_buf(compiler, 1 + 5 + 1);
		FAIL_IF(!buf);

		INC_SIZE(5 + 1);
		*buf++ = 0x68;
		*(sljit_hw*)buf = srcw;
		buf += sizeof(sljit_hw);
	}

	RET();
	return SLJIT_SUCCESS;
}


/* --------------------------------------------------------------------- */
/*  Extend input                                                         */
/* --------------------------------------------------------------------- */

static int emit_mov_int(struct sljit_compiler *compiler, int sign,
	int dst, sljit_w dstw,
	int src, sljit_w srcw)
{
	sljit_ub* code;
	int dst_r;

	compiler->mode32 = 0;

	if (dst == SLJIT_UNUSED && !(src & SLJIT_MEM))
		return SLJIT_SUCCESS; /* Empty instruction. */

	if (src & SLJIT_IMM) {
		if (dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_NO_REGISTERS) {
			if (sign || ((sljit_uw)srcw <= 0x7fffffff)) {
				code = emit_x86_instruction(compiler, 1, SLJIT_IMM, (sljit_w)(sljit_i)srcw, dst, dstw);
				FAIL_IF(!code);
				*code = 0xc7;
				return SLJIT_SUCCESS;
			}
			return emit_load_imm64(compiler, dst, srcw);
		}
		compiler->mode32 = 1;
		code = emit_x86_instruction(compiler, 1, SLJIT_IMM, (sljit_w)(sljit_i)srcw, dst, dstw);
		FAIL_IF(!code);
		*code = 0xc7;
		compiler->mode32 = 0;
		return SLJIT_SUCCESS;
	}

	dst_r = (dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_SAVED_REG3) ? dst : TMP_REGISTER;

	if ((dst & SLJIT_MEM) && (src >= SLJIT_TEMPORARY_REG1 && src <= SLJIT_SAVED_REG3))
		dst_r = src;
	else {
		if (sign) {
			code = emit_x86_instruction(compiler, 1, dst_r, 0, src, srcw);
			FAIL_IF(!code);
			*code++ = 0x63;
		} else {
			compiler->mode32 = 1;
			FAIL_IF(emit_mov(compiler, dst_r, 0, src, srcw));
			compiler->mode32 = 0;
		}
	}

	if (dst & SLJIT_MEM) {
		compiler->mode32 = 1;
		code = emit_x86_instruction(compiler, 1, dst_r, 0, dst, dstw);
		FAIL_IF(!code);
		*code = 0x89;
		compiler->mode32 = 0;
	}

	return SLJIT_SUCCESS;
}
