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

SLJIT_API_FUNC_ATTRIBUTE SLJIT_CONST char* sljit_get_platform_name()
{
	return "PowerPC" SLJIT_CPUINFO;
}

/* Length of an instruction word.
   Both for ppc-32 and ppc-64. */
typedef sljit_ui sljit_ins;

static void ppc_cache_flush(sljit_ins *from, sljit_ins *to)
{
	while (from < to) {
#ifdef __GNUC__
		asm volatile ( "icbi 0, %0" : : "r"(from) );
#else
#error "Must implement icbi"
#endif
		from++;
	}
}

#define TMP_REG1	(SLJIT_NO_REGISTERS + 1)
#define TMP_REG2	(SLJIT_NO_REGISTERS + 2)
#define TMP_REG3	(SLJIT_NO_REGISTERS + 3)
#define ZERO_REG	(SLJIT_NO_REGISTERS + 4)
#define REAL_STACK_PTR	(SLJIT_NO_REGISTERS + 5)

#define TMP_FREG1	(SLJIT_FLOAT_REG4 + 1)
#define TMP_FREG2	(SLJIT_FLOAT_REG4 + 2)

/* --------------------------------------------------------------------- */
/*  Instrucion forms                                                     */
/* --------------------------------------------------------------------- */
#define D(d)		(reg_map[d] << 21)
#define S(s)		(reg_map[s] << 21)
#define A(a)		(reg_map[a] << 16)
#define B(b)		(reg_map[b] << 11)
#define C(c)		(reg_map[c] << 6)
#define FD(fd)		((fd) << 21)
#define FA(fa)		((fa) << 16)
#define FB(fb)		((fb) << 11)
#define FC(fc)		((fc) << 6)
#define IMM(imm)	((imm) & 0xffff)
#define CRD(d)		((d) << 21)

/* Instruction bit sections.
   OE and Rc flag (see ALT_SET_FLAGS). */
#define OERC(flags)	(((flags & ALT_SET_FLAGS) >> 10) | (flags & ALT_SET_FLAGS))
/* Rc flag (see ALT_SET_FLAGS). */
#define RC(flags)	((flags & ALT_SET_FLAGS) >> 10)
#define HI(opcode)	((opcode) << 26)
#define LO(opcode)	((opcode) << 1)

#define ADD		(HI(31) | LO(266))
#define ADDC		(HI(31) | LO(10))
#define ADDE		(HI(31) | LO(138))
#define ADDI		(HI(14))
#define ADDIC		(HI(13))
#define ADDIS		(HI(15))
#define ADDME		(HI(31) | LO(234))
#define AND		(HI(31) | LO(28))
#define ANDI		(HI(28))
#define ANDIS		(HI(29))
#define Bx		(HI(18))
#define BCx		(HI(16))
#define BCCTR		(HI(19) | LO(528) | (3 << 11))
#define BLR		(HI(19) | LO(16) | (0x14 << 21))
#define CNTLZD		(HI(31) | LO(58))
#define CNTLZW		(HI(31) | LO(26))
#define CMP		(HI(31) | LO(0))
#define CMPI		(HI(11))
#define CMPL		(HI(31) | LO(32))
#define CMPLI		(HI(10))
#define CROR		(HI(19) | LO(449))
#define DIVD		(HI(31) | LO(489))
#define DIVDU		(HI(31) | LO(457))
#define DIVW		(HI(31) | LO(491))
#define DIVWU		(HI(31) | LO(459))
#define EXTSB		(HI(31) | LO(954))
#define EXTSH		(HI(31) | LO(922))
#define EXTSW		(HI(31) | LO(986))
#define FABS		(HI(63) | LO(264))
#define FADD		(HI(63) | LO(21))
#define FCMPU		(HI(63) | LO(0))
#define FDIV		(HI(63) | LO(18))
#define FMR		(HI(63) | LO(72))
#define FMUL		(HI(63) | LO(25))
#define FNEG		(HI(63) | LO(40))
#define FSUB		(HI(63) | LO(20))
#define LD		(HI(58) | 0)
#define LFD		(HI(50))
#define LFDUX		(HI(31) | LO(631))
#define LFDX		(HI(31) | LO(599))
#define LWZ		(HI(32))
#define MFCR		(HI(31) | LO(19))
#define MFLR		(HI(31) | LO(339) | 0x80000)
#define MFXER		(HI(31) | LO(339) | 0x10000)
#define MTCTR		(HI(31) | LO(467) | 0x90000)
#define MTLR		(HI(31) | LO(467) | 0x80000)
#define MTXER		(HI(31) | LO(467) | 0x10000)
#define MULHD		(HI(31) | LO(73))
#define MULHDU		(HI(31) | LO(9))
#define MULHW		(HI(31) | LO(75))
#define MULHWU		(HI(31) | LO(11))
#define MULLD		(HI(31) | LO(233))
#define MULLI		(HI(7))
#define MULLW		(HI(31) | LO(235))
#define NEG		(HI(31) | LO(104))
#define NOP		(HI(24))
#define NOR		(HI(31) | LO(124))
#define OR		(HI(31) | LO(444))
#define ORI		(HI(24))
#define ORIS		(HI(25))
#define RLDICL		(HI(30))
#define RLWINM		(HI(21))
#define SLD		(HI(31) | LO(27))
#define SLW		(HI(31) | LO(24))
#define SRAD		(HI(31) | LO(794))
#define SRADI		(HI(31) | LO(413 << 1))
#define SRAW		(HI(31) | LO(792))
#define SRAWI		(HI(31) | LO(824))
#define SRD		(HI(31) | LO(539))
#define SRW		(HI(31) | LO(536))
#define STD		(HI(62) | 0)
#define STDU		(HI(62) | 1)
#define STDUX		(HI(31) | LO(181))
#define STFD		(HI(54))
#define STFDUX		(HI(31) | LO(759))
#define STFDX		(HI(31) | LO(727))
#define STW		(HI(36))
#define STWU		(HI(37))
#define STWUX		(HI(31) | LO(183))
#define SUBF		(HI(31) | LO(40))
#define SUBFC		(HI(31) | LO(8))
#define SUBFE		(HI(31) | LO(136))
#define SUBFIC		(HI(8))
#define XOR		(HI(31) | LO(316))
#define XORI		(HI(26))
#define XORIS		(HI(27))

#define SIMM_MAX	(0x7fff)
#define SIMM_MIN	(-0x8000)
#define UIMM_MAX	(0xffff)

/* SLJIT_LOCALS_REG is not the real stack register, since it must
   point to the head of the stack chain. */
static SLJIT_CONST sljit_ub reg_map[SLJIT_NO_REGISTERS + 6] = {
  0, 3, 4, 5, 6, 7, 29, 28, 27, 26, 25, 31, 8, 9, 10, 30, 1
};

static int push_inst(struct sljit_compiler *compiler, sljit_ins ins)
{
	sljit_ins *ptr = (sljit_ins*)ensure_buf(compiler, sizeof(sljit_ins));
	FAIL_IF(!ptr);
	*ptr = ins;
	compiler->size++;
	return SLJIT_SUCCESS;
}

static SLJIT_INLINE int optimize_jump(struct sljit_jump *jump, sljit_ins *code_ptr, sljit_ins *code)
{
	sljit_w diff;
	sljit_uw target_addr;

	if (jump->flags & SLJIT_REWRITABLE_JUMP)
		return 0;

	if (jump->flags & JUMP_ADDR)
		target_addr = jump->u.target;
	else {
		SLJIT_ASSERT(jump->flags & JUMP_LABEL);
		target_addr = (sljit_uw)(code + jump->u.label->size);
	}
	diff = ((sljit_w)target_addr - (sljit_w)(code_ptr)) & ~0x3l;

	if (jump->flags & UNCOND_B) {
		if (diff <= 0x01ffffff && diff >= -0x02000000) {
			jump->flags |= PATCH_B;
			return 1;
		}
		if (target_addr <= 0x03ffffff) {
			jump->flags |= PATCH_B | ABSOLUTE_B;
			return 1;
		}
	}
	else {
		if (diff <= 0x7fff && diff >= -0x8000) {
			jump->flags |= PATCH_B;
			return 1;
		}
		if (target_addr <= 0xffff) {
			jump->flags |= PATCH_B | ABSOLUTE_B;
			return 1;
		}
	}
	return 0;
}

SLJIT_API_FUNC_ATTRIBUTE void* sljit_generate_code(struct sljit_compiler *compiler)
{
	struct sljit_memory_fragment *buf;
	sljit_ins *code;
	sljit_ins *code_ptr;
	sljit_ins *buf_ptr;
	sljit_ins *buf_end;
	sljit_uw word_count;
	sljit_uw addr;

	struct sljit_label *label;
	struct sljit_jump *jump;
	struct sljit_const *const_;

	CHECK_ERROR_PTR();
	check_sljit_generate_code(compiler);
	reverse_buf(compiler);

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	compiler->size += (compiler->size & 0x1) + (sizeof(struct sljit_function_context) / sizeof(sljit_ins));
#endif
	code = (sljit_ins*)SLJIT_MALLOC_EXEC(compiler->size * sizeof(sljit_ins));
	PTR_FAIL_WITH_EXEC_IF(code);
	buf = compiler->buf;

	code_ptr = code;
	word_count = 0;
	label = compiler->labels;
	jump = compiler->jumps;
	const_ = compiler->consts;
	do {
		buf_ptr = (sljit_ins*)buf->memory;
		buf_end = buf_ptr + (buf->used_size >> 2);
		do {
			*code_ptr = *buf_ptr++;
			SLJIT_ASSERT(!label || label->size >= word_count);
			SLJIT_ASSERT(!jump || jump->addr >= word_count);
			SLJIT_ASSERT(!const_ || const_->addr >= word_count);
			/* These structures are ordered by their address. */
			if (label && label->size == word_count) {
				/* Just recording the address. */
				label->addr = (sljit_uw)code_ptr;
				label->size = code_ptr - code;
				label = label->next;
			}
			if (jump && jump->addr == word_count) {
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
				jump->addr = (sljit_uw)(code_ptr - 3);
#else
				jump->addr = (sljit_uw)(code_ptr - 6);
#endif
				if (optimize_jump(jump, code_ptr, code)) {
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
					code_ptr[-3] = code_ptr[0];
					code_ptr -= 3;
#else
					code_ptr[-6] = code_ptr[0];
					code_ptr -= 6;
#endif
				}
				jump = jump->next;
			}
			if (const_ && const_->addr == word_count) {
				/* Just recording the address. */
				const_->addr = (sljit_uw)code_ptr;
				const_ = const_->next;
			}
			code_ptr ++;
			word_count ++;
		} while (buf_ptr < buf_end);

		buf = buf->next;
	} while (buf);

	if (label && label->size == word_count) {
		label->addr = (sljit_uw)code_ptr;
		label->size = code_ptr - code;
		label = label->next;
	}

	SLJIT_ASSERT(!label);
	SLJIT_ASSERT(!jump);
	SLJIT_ASSERT(!const_);
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	SLJIT_ASSERT(code_ptr - code <= (int)compiler->size - ((compiler->size & 0x1) ? 3 : 2));
#else
	SLJIT_ASSERT(code_ptr - code <= (int)compiler->size);
#endif

	jump = compiler->jumps;
	while (jump) {
		do {
			addr = (jump->flags & JUMP_LABEL) ? jump->u.label->addr : jump->u.target;
			buf_ptr = (sljit_ins*)jump->addr;
			if (jump->flags & PATCH_B) {
				if (jump->flags & UNCOND_B) {
					if (!(jump->flags & ABSOLUTE_B)) {
						addr = addr - jump->addr;
						SLJIT_ASSERT((sljit_w)addr <= 0x01ffffff && (sljit_w)addr >= -0x02000000);
						*buf_ptr = Bx | (addr & 0x03fffffc) | ((*buf_ptr) & 0x1);
					}
					else {
						SLJIT_ASSERT(addr <= 0x03ffffff);
						*buf_ptr = Bx | (addr & 0x03fffffc) | 0x2 | ((*buf_ptr) & 0x1);
					}
				}
				else {
					if (!(jump->flags & ABSOLUTE_B)) {
						addr = addr - jump->addr;
						SLJIT_ASSERT((sljit_w)addr <= 0x7fff && (sljit_w)addr >= -0x8000);
						*buf_ptr = BCx | (addr & 0xfffc) | ((*buf_ptr) & 0x03ff0001);
					}
					else {
						addr = addr & ~0x3l;
						SLJIT_ASSERT(addr <= 0xffff);
						*buf_ptr = BCx | (addr & 0xfffc) | 0x2 | ((*buf_ptr) & 0x03ff0001);
					}

				}
				break;
			}
			/* Set the fields of immediate loads. */
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
			buf_ptr[0] = (buf_ptr[0] & 0xffff0000) | ((addr >> 16) & 0xffff);
			buf_ptr[1] = (buf_ptr[1] & 0xffff0000) | (addr & 0xffff);
#else
			buf_ptr[0] = (buf_ptr[0] & 0xffff0000) | ((addr >> 48) & 0xffff);
			buf_ptr[1] = (buf_ptr[1] & 0xffff0000) | ((addr >> 32) & 0xffff);
			buf_ptr[3] = (buf_ptr[3] & 0xffff0000) | ((addr >> 16) & 0xffff);
			buf_ptr[4] = (buf_ptr[4] & 0xffff0000) | (addr & 0xffff);
#endif
		} while (0);
		jump = jump->next;
	}

	SLJIT_CACHE_FLUSH(code, code_ptr);
	compiler->error = SLJIT_ERR_COMPILED;
	compiler->executable_size = compiler->size * sizeof(sljit_ins);

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	if (((sljit_w)code_ptr) & 0x4)
		code_ptr++;
	sljit_set_function_context(NULL, (struct sljit_function_context*)code_ptr, (sljit_w)code, sljit_generate_code);
	return code_ptr;
#else
	return code;
#endif
}

/* inp_flags: */

/* Creates an index in data_transfer_insts array. */
#define WORD_DATA	0x00
#define BYTE_DATA	0x01
#define HALF_DATA	0x02
#define INT_DATA	0x03
#define SIGNED_DATA	0x04
#define LOAD_DATA	0x08
#define WRITE_BACK	0x10
#define INDEXED		0x20

#define MEM_MASK	0x3f

/* Other inp_flags. */

#define ARG_TEST	0x000100
/* Integer opertion and set flags -> requires exts on 64 bit systems. */
#define ALT_SIGN_EXT	0x000200
/* This flag affects the RC() and OERC() macros. */
#define ALT_SET_FLAGS	0x000400
#define ALT_FORM1	0x010000
#define ALT_FORM2	0x020000
#define ALT_FORM3	0x040000
#define ALT_FORM4	0x080000
#define ALT_FORM5	0x100000
#define ALT_FORM6	0x200000

/* Source and destination is register. */
#define REG_DEST	0x000001
#define REG1_SOURCE	0x000002
#define REG2_SOURCE	0x000004
/* getput_arg_fast returned true. */
#define FAST_DEST	0x000008
/* Multiple instructions are required. */
#define SLOW_DEST	0x000010
/*
ALT_SIGN_EXT		0x000200
ALT_SET_FLAGS		0x000400
ALT_FORM1		0x010000
...
ALT_FORM6		0x200000 */

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
#include "sljitNativePPC_32.c"
#else
#include "sljitNativePPC_64.c"
#endif

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
#define STACK_STORE	STW
#define STACK_LOAD	LWZ
#else
#define STACK_STORE	STD
#define STACK_LOAD	LD
#endif

static int emit_op(struct sljit_compiler *compiler, int op, int inp_flags,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w);

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_enter(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	CHECK_ERROR();
	check_sljit_emit_enter(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;
	compiler->has_locals = local_size > 0;

	FAIL_IF(push_inst(compiler, MFLR | D(0)));
	if (compiler->has_locals)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_LOCALS_REG) | A(REAL_STACK_PTR) | IMM(-(int)(sizeof(sljit_w))) ));
	FAIL_IF(push_inst(compiler, STACK_STORE | S(ZERO_REG) | A(REAL_STACK_PTR) | IMM(-2 * (int)(sizeof(sljit_w))) ));
	if (saveds >= 1)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_SAVED_REG1) | A(REAL_STACK_PTR) | IMM(-3 * (int)(sizeof(sljit_w))) ));
	if (saveds >= 2)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_SAVED_REG2) | A(REAL_STACK_PTR) | IMM(-4 * (int)(sizeof(sljit_w))) ));
	if (saveds >= 3)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_SAVED_REG3) | A(REAL_STACK_PTR) | IMM(-5 * (int)(sizeof(sljit_w))) ));
	if (saveds >= 4)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_SAVED_EREG1) | A(REAL_STACK_PTR) | IMM(-6 * (int)(sizeof(sljit_w))) ));
	if (saveds >= 5)
		FAIL_IF(push_inst(compiler, STACK_STORE | S(SLJIT_SAVED_EREG2) | A(REAL_STACK_PTR) | IMM(-7 * (int)(sizeof(sljit_w))) ));
	FAIL_IF(push_inst(compiler, STACK_STORE | S(0) | A(REAL_STACK_PTR) | IMM(sizeof(sljit_w)) ));

	FAIL_IF(push_inst(compiler, ADDI | D(ZERO_REG) | A(0) | 0));
	if (args >= 1)
		FAIL_IF(push_inst(compiler, OR | S(SLJIT_TEMPORARY_REG1) | A(SLJIT_SAVED_REG1) | B(SLJIT_TEMPORARY_REG1)));
	if (args >= 2)
		FAIL_IF(push_inst(compiler, OR | S(SLJIT_TEMPORARY_REG2) | A(SLJIT_SAVED_REG2) | B(SLJIT_TEMPORARY_REG2)));
	if (args >= 3)
		FAIL_IF(push_inst(compiler, OR | S(SLJIT_TEMPORARY_REG3) | A(SLJIT_SAVED_REG3) | B(SLJIT_TEMPORARY_REG3)));

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
	compiler->local_size = (2 + saveds + 2) * sizeof(sljit_w) + local_size;
#else
	compiler->local_size = (2 + saveds + 7 + 8) * sizeof(sljit_w) + local_size;
#endif
	compiler->local_size = (compiler->local_size + 15) & ~0xf;

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
	if (compiler->local_size <= SIMM_MAX)
		FAIL_IF(push_inst(compiler, STWU | S(REAL_STACK_PTR) | A(REAL_STACK_PTR) | IMM(-compiler->local_size)));
	else {
		FAIL_IF(load_immediate(compiler, 0, -compiler->local_size));
		FAIL_IF(push_inst(compiler, STWUX | S(REAL_STACK_PTR) | A(REAL_STACK_PTR) | B(0)));
	}
	if (compiler->has_locals)
		FAIL_IF(push_inst(compiler, ADDI | D(SLJIT_LOCALS_REG) | A(REAL_STACK_PTR) | IMM(2 * sizeof(sljit_w))));
#else
	if (compiler->local_size <= SIMM_MAX)
		FAIL_IF(push_inst(compiler, STDU | S(REAL_STACK_PTR) | A(REAL_STACK_PTR) | IMM(-compiler->local_size)));
	else {
		FAIL_IF(load_immediate(compiler, 0, -compiler->local_size));
		FAIL_IF(push_inst(compiler, STDUX | S(REAL_STACK_PTR) | A(REAL_STACK_PTR) | B(0)));
	}
	if (compiler->has_locals)
		FAIL_IF(push_inst(compiler, ADDI | D(SLJIT_LOCALS_REG) | A(REAL_STACK_PTR) | IMM((7 + 8) * sizeof(sljit_w))));
#endif

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_context(struct sljit_compiler *compiler, int args, int temporaries, int saveds, int local_size)
{
	CHECK_ERROR_VOID();
	check_sljit_set_context(compiler, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;

	compiler->has_locals = local_size > 0;
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
	compiler->local_size = (2 + saveds + 2) * sizeof(sljit_w) + local_size;
#else
	compiler->local_size = (2 + saveds + 7 + 8) * sizeof(sljit_w) + local_size;
#endif
	compiler->local_size = (compiler->local_size + 15) & ~0xf;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_return(struct sljit_compiler *compiler, int op, int src, sljit_w srcw)
{
	CHECK_ERROR();
	check_sljit_emit_return(compiler, op, src, srcw);

	FAIL_IF(emit_mov_before_return(compiler, op, src, srcw));

	if (compiler->local_size <= SIMM_MAX)
		FAIL_IF(push_inst(compiler, ADDI | D(REAL_STACK_PTR) | A(REAL_STACK_PTR) | IMM(compiler->local_size)));
	else {
		FAIL_IF(load_immediate(compiler, 0, compiler->local_size));
		FAIL_IF(push_inst(compiler, ADD | D(REAL_STACK_PTR) | A(REAL_STACK_PTR) | B(0)));
	}

	FAIL_IF(push_inst(compiler, STACK_LOAD | D(0) | A(REAL_STACK_PTR) | IMM(sizeof(sljit_w))));
	if (compiler->saveds >= 5)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_SAVED_EREG2) | A(REAL_STACK_PTR) | IMM(-7 * (int)(sizeof(sljit_w))) ));
	if (compiler->saveds >= 4)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_SAVED_EREG1) | A(REAL_STACK_PTR) | IMM(-6 * (int)(sizeof(sljit_w))) ));
	if (compiler->saveds >= 3)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_SAVED_REG3) | A(REAL_STACK_PTR) | IMM(-5 * (int)(sizeof(sljit_w))) ));
	if (compiler->saveds >= 2)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_SAVED_REG2) | A(REAL_STACK_PTR) | IMM(-4 * (int)(sizeof(sljit_w))) ));
	if (compiler->saveds >= 1)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_SAVED_REG1) | A(REAL_STACK_PTR) | IMM(-3 * (int)(sizeof(sljit_w))) ));
	FAIL_IF(push_inst(compiler, STACK_LOAD | D(ZERO_REG) | A(REAL_STACK_PTR) | IMM(-2 * (int)(sizeof(sljit_w))) ));
	if (compiler->has_locals)
		FAIL_IF(push_inst(compiler, STACK_LOAD | D(SLJIT_LOCALS_REG) | A(REAL_STACK_PTR) | IMM(-(int)(sizeof(sljit_w))) ));

	FAIL_IF(push_inst(compiler, MTLR | S(0)));
	FAIL_IF(push_inst(compiler, BLR));

	return SLJIT_SUCCESS;
}

#undef STACK_STORE
#undef STACK_LOAD

/* --------------------------------------------------------------------- */
/*  Operators                                                            */
/* --------------------------------------------------------------------- */

/* i/x - immediate/indexed form
   n/w - no write-back / write-back (1 bit)
   s/l - store/load (1 bit)
   u/s - signed/unsigned (1 bit)
   w/b/h/i - word/byte/half/int allowed (2 bit)
   It contans 32 items, but not all are different. */

/* 64 bit only: [reg+imm] must be aligned to 4 bytes. */
#define ADDR_MODE2	0x10000
/* 64-bit only: there is no lwau instruction. */
#define UPDATE_REQ	0x20000

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
#define ARCH_DEPEND(a, b)	a
#define GET_INST_CODE(inst)	(inst)
#else
#define ARCH_DEPEND(a, b)	b
#define GET_INST_CODE(index)	((inst) & ~(ADDR_MODE2 | UPDATE_REQ))
#endif

static SLJIT_CONST sljit_ins data_transfer_insts[64] = {

/* No write-back. */

/* i n s u w */ ARCH_DEPEND(HI(36) /* stw */, HI(62) | ADDR_MODE2 | 0x0 /* std */),
/* i n s u b */ HI(38) /* stb */,
/* i n s u h */ HI(44) /* sth*/,
/* i n s u i */ HI(36) /* stw */,

/* i n s s w */ ARCH_DEPEND(HI(36) /* stw */, HI(62) | ADDR_MODE2 | 0x0 /* std */),
/* i n s s b */ HI(38) /* stb */,
/* i n s s h */ HI(44) /* sth*/,
/* i n s s i */ HI(36) /* stw */,

/* i n l u w */ ARCH_DEPEND(HI(32) /* lwz */, HI(58) | ADDR_MODE2 | 0x0 /* ld */),
/* i n l u b */ HI(34) /* lbz */,
/* i n l u h */ HI(40) /* lhz */,
/* i n l u i */ HI(32) /* lwz */,

/* i n l s w */ ARCH_DEPEND(HI(32) /* lwz */, HI(58) | ADDR_MODE2 | 0x0 /* ld */),
/* i n l s b */ HI(34) /* lbz */ /* EXTS_REQ */,
/* i n l s h */ HI(42) /* lha */,
/* i n l s i */ ARCH_DEPEND(HI(32) /* lwz */, HI(58) | ADDR_MODE2 | 0x2 /* lwa */),

/* Write-back. */

/* i w s u w */ ARCH_DEPEND(HI(37) /* stwu */, HI(62) | ADDR_MODE2 | 0x1 /* stdu */),
/* i w s u b */ HI(39) /* stbu */,
/* i w s u h */ HI(45) /* sthu */,
/* i w s u i */ HI(37) /* stwu */,

/* i w s s w */ ARCH_DEPEND(HI(37) /* stwu */, HI(62) | ADDR_MODE2 | 0x1 /* stdu */),
/* i w s s b */ HI(39) /* stbu */,
/* i w s s h */ HI(45) /* sthu */,
/* i w s s i */ HI(37) /* stwu */,

/* i w l u w */ ARCH_DEPEND(HI(33) /* lwzu */, HI(58) | ADDR_MODE2 | 0x1 /* ldu */),
/* i w l u b */ HI(35) /* lbzu */,
/* i w l u h */ HI(41) /* lhzu */,
/* i w l u i */ HI(33) /* lwzu */,

/* i w l s w */ ARCH_DEPEND(HI(33) /* lwzu */, HI(58) | ADDR_MODE2 | 0x1 /* ldu */),
/* i w l s b */ HI(35) /* lbzu */ /* EXTS_REQ */,
/* i w l s h */ HI(43) /* lhau */,
/* i w l s i */ ARCH_DEPEND(HI(33) /* lwzu */, HI(58) | ADDR_MODE2 | UPDATE_REQ | 0x2 /* lwa */),

/* ---------- */
/*  Indexed   */
/* ---------- */

/* No write-back. */

/* x n s u w */ ARCH_DEPEND(HI(31) | LO(151) /* stwx */, HI(31) | LO(149) /* stdx */),
/* x n s u b */ HI(31) | LO(215) /* stbx */,
/* x n s u h */ HI(31) | LO(407) /* sthx */,
/* x n s u i */ HI(31) | LO(151) /* stwx */,

/* x n s s w */ ARCH_DEPEND(HI(31) | LO(151) /* stwx */, HI(31) | LO(149) /* stdx */),
/* x n s s b */ HI(31) | LO(215) /* stbx */,
/* x n s s h */ HI(31) | LO(407) /* sthx */,
/* x n s s i */ HI(31) | LO(151) /* stwx */,

/* x n l u w */ ARCH_DEPEND(HI(31) | LO(23) /* lwzx */, HI(31) | LO(21) /* ldx */),
/* x n l u b */ HI(31) | LO(87) /* lbzx */,
/* x n l u h */ HI(31) | LO(279) /* lhzx */,
/* x n l u i */ HI(31) | LO(23) /* lwzx */,

/* x n l s w */ ARCH_DEPEND(HI(31) | LO(23) /* lwzx */, HI(31) | LO(21) /* ldx */),
/* x n l s b */ HI(31) | LO(87) /* lbzx */ /* EXTS_REQ */,
/* x n l s h */ HI(31) | LO(343) /* lhax */,
/* x n l s i */ ARCH_DEPEND(HI(31) | LO(23) /* lwzx */, HI(31) | LO(341) /* lwax */),

/* Write-back. */

/* x w s u w */ ARCH_DEPEND(HI(31) | LO(183) /* stwux */, HI(31) | LO(181) /* stdux */),
/* x w s u b */ HI(31) | LO(247) /* stbux */,
/* x w s u h */ HI(31) | LO(439) /* sthux */,
/* x w s u i */ HI(31) | LO(183) /* stwux */,

/* x w s s w */ ARCH_DEPEND(HI(31) | LO(183) /* stwux */, HI(31) | LO(181) /* stdux */),
/* x w s s b */ HI(31) | LO(247) /* stbux */,
/* x w s s h */ HI(31) | LO(439) /* sthux */,
/* x w s s i */ HI(31) | LO(183) /* stwux */,

/* x w l u w */ ARCH_DEPEND(HI(31) | LO(55) /* lwzux */, HI(31) | LO(53) /* ldux */),
/* x w l u b */ HI(31) | LO(119) /* lbzux */,
/* x w l u h */ HI(31) | LO(311) /* lhzux */,
/* x w l u i */ HI(31) | LO(55) /* lwzux */,

/* x w l s w */ ARCH_DEPEND(HI(31) | LO(55) /* lwzux */, HI(31) | LO(53) /* ldux */),
/* x w l s b */ HI(31) | LO(119) /* lbzux */ /* EXTS_REQ */,
/* x w l s h */ HI(31) | LO(375) /* lhaux */,
/* x w l s i */ ARCH_DEPEND(HI(31) | LO(55) /* lwzux */, HI(31) | LO(373) /* lwaux */)

};

#undef ARCH_DEPEND

/* Simple cases, (no caching is required). */
static int getput_arg_fast(struct sljit_compiler *compiler, int inp_flags, int reg, int arg, sljit_w argw)
{
	sljit_ins inst;
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	int tmp_reg;
#endif

	SLJIT_ASSERT(arg & SLJIT_MEM);
	if (!(arg & 0xf)) {
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
		if (argw <= SIMM_MAX && argw >= SIMM_MIN) {
			if (inp_flags & ARG_TEST)
				return 1;

			inst = data_transfer_insts[(inp_flags & ~WRITE_BACK) & MEM_MASK];
			SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
			push_inst(compiler, GET_INST_CODE(inst) | D(reg) | IMM(argw));
			return -1;
		}
#else
		inst = data_transfer_insts[(inp_flags & ~WRITE_BACK) & MEM_MASK];
		if (argw <= SIMM_MAX && argw >= SIMM_MIN &&
				(!(inst & ADDR_MODE2) || (argw & 0x3) == 0)) {
			if (inp_flags & ARG_TEST)
				return 1;

			push_inst(compiler, GET_INST_CODE(inst) | D(reg) | IMM(argw));
			return -1;
		}
#endif
		return (inp_flags & ARG_TEST) ? SLJIT_SUCCESS : 0;
	}

	if (!(arg & 0xf0)) {
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
		if (argw <= SIMM_MAX && argw >= SIMM_MIN) {
			if (inp_flags & ARG_TEST)
				return 1;

			inst = data_transfer_insts[inp_flags & MEM_MASK];
			SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
			push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | IMM(argw));
			return -1;
		}
#else
		inst = data_transfer_insts[inp_flags & MEM_MASK];
		if (argw <= SIMM_MAX && argw >= SIMM_MIN && (!(inst & ADDR_MODE2) || (argw & 0x3) == 0)) {
			if (inp_flags & ARG_TEST)
				return 1;

			if ((inp_flags & WRITE_BACK) && (inst & UPDATE_REQ)) {
				tmp_reg = (inp_flags & LOAD_DATA) ? (arg & 0xf) : TMP_REG3;
				if (push_inst(compiler, ADDI | D(tmp_reg) | A(arg & 0xf) | IMM(argw)))
					return -1;
				arg = tmp_reg | SLJIT_MEM;
				argw = 0;
			}
			push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | IMM(argw));
			return -1;
		}
#endif
	}
	else if (!(argw & 0x3)) {
		if (inp_flags & ARG_TEST)
			return 1;
		inst = data_transfer_insts[(inp_flags | INDEXED) & MEM_MASK];
		SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
		push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | B((arg >> 4) & 0xf));
		return -1;
	}
	return (inp_flags & ARG_TEST) ? SLJIT_SUCCESS : 0;
}

/* See getput_arg below.
   Note: can_cache is called only for binary operators. Those operator always
   uses word arguments without write back. */
static int can_cache(int arg, sljit_w argw, int next_arg, sljit_w next_argw)
{
	SLJIT_ASSERT(arg & SLJIT_MEM);
	SLJIT_ASSERT(next_arg & SLJIT_MEM);

	if (!(arg & 0xf)) {
		if ((next_arg & SLJIT_MEM) && ((sljit_uw)argw - (sljit_uw)next_argw <= SIMM_MAX || (sljit_uw)next_argw - (sljit_uw)argw <= SIMM_MAX))
			return 1;
		return 0;
	}

	if (arg & 0xf0)
		return 0;

	if (argw <= SIMM_MAX && argw >= SIMM_MIN) {
		if (arg == next_arg && (next_argw >= SIMM_MAX && next_argw <= SIMM_MIN))
			return 1;
	}

	if (arg == next_arg && ((sljit_uw)argw - (sljit_uw)next_argw <= SIMM_MAX || (sljit_uw)next_argw - (sljit_uw)argw <= SIMM_MAX))
		return 1;

	return 0;
}

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
#define ADJUST_CACHED_IMM(imm) \
	if ((inst & ADDR_MODE2) && (imm & 0x3)) { \
		/* Adjust cached value. Fortunately this is really a rare case */ \
		compiler->cache_argw += imm & 0x3; \
		FAIL_IF(push_inst(compiler, ADDI | D(TMP_REG3) | A(TMP_REG3) | (imm & 0x3))); \
		imm &= ~0x3; \
	}
#else
#define ADJUST_CACHED_IMM(imm)
#endif

/* Emit the necessary instructions. See can_cache above. */
static int getput_arg(struct sljit_compiler *compiler, int inp_flags, int reg, int arg, sljit_w argw, int next_arg, sljit_w next_argw)
{
	int tmp_r;
	sljit_ins inst;

	SLJIT_ASSERT(arg & SLJIT_MEM);

	tmp_r = (inp_flags & LOAD_DATA) ? reg : TMP_REG3;
	if ((arg & 0xf) == tmp_r) {
		/* Special case for "mov reg, [reg, ... ]".
		   Caching would not happen anyway. */
		tmp_r = TMP_REG3;
		compiler->cache_arg = 0;
		compiler->cache_argw = 0;
	}

	if (!(arg & 0xf)) {
		inst = data_transfer_insts[(inp_flags & ~WRITE_BACK) & MEM_MASK];
		if ((compiler->cache_arg & SLJIT_IMM) && (((sljit_uw)argw - (sljit_uw)compiler->cache_argw) <= SIMM_MAX || ((sljit_uw)compiler->cache_argw - (sljit_uw)argw) <= SIMM_MAX)) {
			argw = argw - compiler->cache_argw;
			ADJUST_CACHED_IMM(argw);
			SLJIT_ASSERT(!(inst & UPDATE_REQ));
			return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(TMP_REG3) | IMM(argw));
		}

		if ((next_arg & SLJIT_MEM) && (argw - next_argw <= SIMM_MAX || next_argw - argw <= SIMM_MAX)) {
			SLJIT_ASSERT(inp_flags & LOAD_DATA);

			compiler->cache_arg = SLJIT_IMM;
			compiler->cache_argw = argw;
			tmp_r = TMP_REG3;
		}

		FAIL_IF(load_immediate(compiler, tmp_r, argw));
		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(tmp_r));
	}

	if (SLJIT_UNLIKELY(arg & 0xf0)) {
		argw &= 0x3;
		/* Otherwise getput_arg_fast would capture it. */
		SLJIT_ASSERT(argw);
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
		FAIL_IF(push_inst(compiler, RLWINM | S((arg >> 4) & 0xf) | A(tmp_r) | (argw << 11) | ((31 - argw) << 1)));
#else
		FAIL_IF(push_inst(compiler, RLDI(tmp_r, (arg >> 4) & 0xf, argw, 63 - argw, 1)));
#endif
		inst = data_transfer_insts[(inp_flags | INDEXED) & MEM_MASK];
		SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | B(tmp_r));
	}

	inst = data_transfer_insts[inp_flags & MEM_MASK];

	if (compiler->cache_arg == arg && ((sljit_uw)argw - (sljit_uw)compiler->cache_argw <= SIMM_MAX || (sljit_uw)compiler->cache_argw - (sljit_uw)argw <= SIMM_MAX)) {
		SLJIT_ASSERT(!(inp_flags & WRITE_BACK));
		argw = argw - compiler->cache_argw;
		ADJUST_CACHED_IMM(argw);
		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(TMP_REG3) | IMM(argw));
	}

	if ((compiler->cache_arg & SLJIT_IMM) && compiler->cache_argw == argw) {
		inst = data_transfer_insts[(inp_flags | INDEXED) & MEM_MASK];
		SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | B(TMP_REG3));
	}

	if (argw == next_argw && (next_arg & SLJIT_MEM)) {
		SLJIT_ASSERT(inp_flags & LOAD_DATA);
		FAIL_IF(load_immediate(compiler, TMP_REG3, argw));

		compiler->cache_arg = SLJIT_IMM;
		compiler->cache_argw = argw;

		inst = data_transfer_insts[(inp_flags | INDEXED) & MEM_MASK];
		SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | B(TMP_REG3));
	}

	if (arg == next_arg && !(inp_flags & WRITE_BACK) && ((sljit_uw)argw - (sljit_uw)next_argw <= SIMM_MAX || (sljit_uw)next_argw - (sljit_uw)argw <= SIMM_MAX)) {
		SLJIT_ASSERT(inp_flags & LOAD_DATA);
		FAIL_IF(load_immediate(compiler, TMP_REG3, argw));
		FAIL_IF(push_inst(compiler, ADD | D(TMP_REG3) | A(TMP_REG3) | B(arg & 0xf)));

		compiler->cache_arg = arg;
		compiler->cache_argw = argw;

		return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(TMP_REG3));
	}

	/* Get the indexed version instead of the normal one. */
	inst = data_transfer_insts[(inp_flags | INDEXED) & MEM_MASK];
	SLJIT_ASSERT(!(inst & (ADDR_MODE2 | UPDATE_REQ)));
	FAIL_IF(load_immediate(compiler, tmp_r, argw));
	return push_inst(compiler, GET_INST_CODE(inst) | D(reg) | A(arg & 0xf) | B(tmp_r));
}

static int emit_op(struct sljit_compiler *compiler, int op, int inp_flags,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w)
{
	/* arg1 goes to TMP_REG1 or src reg
	   arg2 goes to TMP_REG2, imm or src reg
	   TMP_REG3 can be used for caching
	   result goes to TMP_REG2, so put result can use TMP_REG1 and TMP_REG3. */
	int dst_r;
	int src1_r;
	int src2_r;
	int sugg_src2_r = TMP_REG2;
	int flags = inp_flags & (ALT_FORM1 | ALT_FORM2 | ALT_FORM3 | ALT_FORM4 | ALT_FORM5 | ALT_FORM6 | ALT_SIGN_EXT | ALT_SET_FLAGS);

	compiler->cache_arg = 0;
	compiler->cache_argw = 0;

	/* Destination check. */
	if (dst >= SLJIT_TEMPORARY_REG1 && dst <= ZERO_REG) {
		dst_r = dst;
		flags |= REG_DEST;
		if (op >= SLJIT_MOV && op <= SLJIT_MOVU_SI)
			sugg_src2_r = dst_r;
	}
	else if (dst == SLJIT_UNUSED) {
		if (op >= SLJIT_MOV && op <= SLJIT_MOVU_SI && !(src2 & SLJIT_MEM))
			return SLJIT_SUCCESS;
		dst_r = TMP_REG2;
	}
	else {
		SLJIT_ASSERT(dst & SLJIT_MEM);
		if (getput_arg_fast(compiler, inp_flags | ARG_TEST, TMP_REG2, dst, dstw)) {
			flags |= FAST_DEST;
			dst_r = TMP_REG2;
		}
		else {
			flags |= SLOW_DEST;
			dst_r = 0;
		}
	}

	/* Source 1. */
	if (src1 >= SLJIT_TEMPORARY_REG1 && src1 <= ZERO_REG) {
		src1_r = src1;
		flags |= REG1_SOURCE;
	}
	else if (src1 & SLJIT_IMM) {
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		if ((inp_flags & 0x3) == INT_DATA) {
			if (inp_flags & SIGNED_DATA)
				src1w = (signed int)src1w;
			else
				src1w = (unsigned int)src1w;
		}
#endif
		FAIL_IF(load_immediate(compiler, TMP_REG1, src1w));
		src1_r = TMP_REG1;
	}
	else if (getput_arg_fast(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w)) {
		FAIL_IF(compiler->error);
		src1_r = TMP_REG1;
	}
	else
		src1_r = 0;

	/* Source 2. */
	if (src2 >= SLJIT_TEMPORARY_REG1 && src2 <= ZERO_REG) {
		src2_r = src2;
		flags |= REG2_SOURCE;
		if (!(flags & REG_DEST) && op >= SLJIT_MOV && op <= SLJIT_MOVU_SI)
			dst_r = src2_r;
	}
	else if (src2 & SLJIT_IMM) {
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		if ((inp_flags & 0x3) == INT_DATA) {
			if (inp_flags & SIGNED_DATA)
				src2w = (signed int)src2w;
			else
				src2w = (unsigned int)src2w;
		}
#endif
		FAIL_IF(load_immediate(compiler, sugg_src2_r, src2w));
		src2_r = sugg_src2_r;
	}
	else if (getput_arg_fast(compiler, inp_flags | LOAD_DATA, sugg_src2_r, src2, src2w)) {
		FAIL_IF(compiler->error);
		src2_r = sugg_src2_r;
	}
	else
		src2_r = 0;

	/* src1_r, src2_r and dst_r can be zero (=unprocessed).
	   All arguments are complex addressing modes, and it is a binary operator. */
	if (src1_r == 0 && src2_r == 0 && dst_r == 0) {
		if (!can_cache(src1, src1w, src2, src2w) && can_cache(src1, src1w, dst, dstw)) {
			FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG2, src2, src2w, src1, src1w));
			FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w, dst, dstw));
		}
		else {
			FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w, src2, src2w));
			FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG2, src2, src2w, dst, dstw));
		}
		src1_r = TMP_REG1;
		src2_r = TMP_REG2;
	}
	else if (src1_r == 0 && src2_r == 0) {
		FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w, src2, src2w));
		src1_r = TMP_REG1;
	}
	else if (src1_r == 0 && dst_r == 0) {
		FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w, dst, dstw));
		src1_r = TMP_REG1;
	}
	else if (src2_r == 0 && dst_r == 0) {
		FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, sugg_src2_r, src2, src2w, dst, dstw));
		src2_r = sugg_src2_r;
	}

	if (dst_r == 0)
		dst_r = TMP_REG2;

	if (src1_r == 0) {
		FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, TMP_REG1, src1, src1w, 0, 0));
		src1_r = TMP_REG1;
	}

	if (src2_r == 0) {
		FAIL_IF(getput_arg(compiler, inp_flags | LOAD_DATA, sugg_src2_r, src2, src2w, 0, 0));
		src2_r = sugg_src2_r;
	}

	FAIL_IF(emit_single_op(compiler, op, flags, dst_r, src1_r, src2_r));

	if (flags & (FAST_DEST | SLOW_DEST)) {
		if (flags & FAST_DEST)
			FAIL_IF(getput_arg_fast(compiler, inp_flags, dst_r, dst, dstw));
		else
			FAIL_IF(getput_arg(compiler, inp_flags, dst_r, dst, dstw, 0, 0));
	}
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op0(struct sljit_compiler *compiler, int op)
{
	CHECK_ERROR();
	check_sljit_emit_op0(compiler, op);

	switch (GET_OPCODE(op)) {
	case SLJIT_BREAKPOINT:
	case SLJIT_NOP:
		return push_inst(compiler, NOP);
		break;
	case SLJIT_UMUL:
	case SLJIT_SMUL:
		FAIL_IF(push_inst(compiler, OR | S(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG1)));
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		FAIL_IF(push_inst(compiler, MULLD | D(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2)));
		return push_inst(compiler, (GET_OPCODE(op) == SLJIT_UMUL ? MULHDU : MULHD) | D(SLJIT_TEMPORARY_REG2) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2));
#else
		FAIL_IF(push_inst(compiler, MULLW | D(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2)));
		return push_inst(compiler, (GET_OPCODE(op) == SLJIT_UMUL ? MULHWU : MULHW) | D(SLJIT_TEMPORARY_REG2) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2));
#endif
	case SLJIT_UDIV:
	case SLJIT_SDIV:
		FAIL_IF(push_inst(compiler, OR | S(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG1)));
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		if (op & SLJIT_INT_OP) {
			FAIL_IF(push_inst(compiler, (GET_OPCODE(op) == SLJIT_UDIV ? DIVWU : DIVW) | D(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2)));
			FAIL_IF(push_inst(compiler, MULLW | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG1) | B(SLJIT_TEMPORARY_REG2)));
			return push_inst(compiler, SUBF | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG2) | B(TMP_REG1));
		}
		FAIL_IF(push_inst(compiler, (GET_OPCODE(op) == SLJIT_UDIV ? DIVDU : DIVD) | D(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2)));
		FAIL_IF(push_inst(compiler, MULLD | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG1) | B(SLJIT_TEMPORARY_REG2)));
		return push_inst(compiler, SUBF | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG2) | B(TMP_REG1));
#else
		FAIL_IF(push_inst(compiler, (GET_OPCODE(op) == SLJIT_UDIV ? DIVWU : DIVW) | D(SLJIT_TEMPORARY_REG1) | A(TMP_REG1) | B(SLJIT_TEMPORARY_REG2)));
		FAIL_IF(push_inst(compiler, MULLW | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG1) | B(SLJIT_TEMPORARY_REG2)));
		return push_inst(compiler, SUBF | D(SLJIT_TEMPORARY_REG2) | A(SLJIT_TEMPORARY_REG2) | B(TMP_REG1));
#endif
	}

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op1(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src, sljit_w srcw)
{
	int inp_flags = GET_FLAGS(op) ? ALT_SET_FLAGS : 0;

	CHECK_ERROR();
	check_sljit_emit_op1(compiler, op, dst, dstw, src, srcw);

	if ((src & SLJIT_IMM) && srcw == 0)
		src = ZERO_REG;

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	if (op & SLJIT_INT_OP) {
		inp_flags |= INT_DATA | SIGNED_DATA;
		if (src & SLJIT_IMM)
			srcw = (int)srcw;
	}
#endif
	if (op & SLJIT_SET_O)
		FAIL_IF(push_inst(compiler, MTXER | S(ZERO_REG)));

	switch (GET_OPCODE(op)) {
	case SLJIT_MOV:
		return emit_op(compiler, SLJIT_MOV, inp_flags | WORD_DATA, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOV_UI:
		return emit_op(compiler, SLJIT_MOV_UI, inp_flags | INT_DATA, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOV_SI:
		return emit_op(compiler, SLJIT_MOV_SI, inp_flags | INT_DATA | SIGNED_DATA, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOV_UB:
		return emit_op(compiler, SLJIT_MOV_UB, inp_flags | BYTE_DATA, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (unsigned char)srcw : srcw);

	case SLJIT_MOV_SB:
		return emit_op(compiler, SLJIT_MOV_SB, inp_flags | BYTE_DATA | SIGNED_DATA, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (signed char)srcw : srcw);

	case SLJIT_MOV_UH:
		return emit_op(compiler, SLJIT_MOV_UH, inp_flags | HALF_DATA, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (unsigned short)srcw : srcw);

	case SLJIT_MOV_SH:
		return emit_op(compiler, SLJIT_MOV_SH, inp_flags | HALF_DATA | SIGNED_DATA, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (signed short)srcw : srcw);

	case SLJIT_MOVU:
		return emit_op(compiler, SLJIT_MOV, inp_flags | WORD_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOVU_UI:
		return emit_op(compiler, SLJIT_MOV_UI, inp_flags | INT_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOVU_SI:
		return emit_op(compiler, SLJIT_MOV_SI, inp_flags | INT_DATA | SIGNED_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_MOVU_UB:
		return emit_op(compiler, SLJIT_MOV_UB, inp_flags | BYTE_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (unsigned char)srcw : srcw);

	case SLJIT_MOVU_SB:
		return emit_op(compiler, SLJIT_MOV_SB, inp_flags | BYTE_DATA | SIGNED_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (signed char)srcw : srcw);

	case SLJIT_MOVU_UH:
		return emit_op(compiler, SLJIT_MOV_UH, inp_flags | HALF_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (unsigned short)srcw : srcw);

	case SLJIT_MOVU_SH:
		return emit_op(compiler, SLJIT_MOV_SH, inp_flags | HALF_DATA | SIGNED_DATA | WRITE_BACK, dst, dstw, TMP_REG1, 0, src, (src & SLJIT_IMM) ? (signed short)srcw : srcw);

	case SLJIT_NOT:
		return emit_op(compiler, SLJIT_NOT, inp_flags, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_NEG:
		return emit_op(compiler, SLJIT_NEG, inp_flags, dst, dstw, TMP_REG1, 0, src, srcw);

	case SLJIT_CLZ:
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		return emit_op(compiler, SLJIT_CLZ, inp_flags | (!(op & SLJIT_INT_OP) ? 0 : ALT_FORM1), dst, dstw, TMP_REG1, 0, src, srcw);
#else
		return emit_op(compiler, SLJIT_CLZ, inp_flags, dst, dstw, TMP_REG1, 0, src, srcw);
#endif
	}

	return SLJIT_SUCCESS;
}

#define TEST_SL_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && (srcw) <= SIMM_MAX && (srcw) >= SIMM_MIN)

#define TEST_UL_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && !((srcw) & ~0xffff))

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
#define TEST_SH_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && !((srcw) & 0xffff) && (srcw) <= SLJIT_W(0x7fffffff) && (srcw) >= SLJIT_W(-0x80000000))
#else
#define TEST_SH_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && !((srcw) & 0xffff))
#endif

#define TEST_UH_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && !((srcw) & ~0xffff0000))

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
#define TEST_ADD_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && (srcw) <= SLJIT_W(0x7fff7fff) && (srcw) >= SLJIT_W(-0x80000000))
#else
#define TEST_ADD_IMM(src, srcw) \
	((src) & SLJIT_IMM)
#endif

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
#define TEST_UI_IMM(src, srcw) \
	(((src) & SLJIT_IMM) && !((srcw) & ~0xffffffff))
#else
#define TEST_UI_IMM(src, srcw) \
	((src) & SLJIT_IMM)
#endif

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op2(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w)
{
	int inp_flags = GET_FLAGS(op) ? ALT_SET_FLAGS : 0;

	CHECK_ERROR();
	check_sljit_emit_op2(compiler, op, dst, dstw, src1, src1w, src2, src2w);

	if ((src1 & SLJIT_IMM) && src1w == 0)
		src1 = ZERO_REG;
	if ((src2 & SLJIT_IMM) && src2w == 0)
		src2 = ZERO_REG;

#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	if (op & SLJIT_INT_OP) {
		inp_flags |= INT_DATA | SIGNED_DATA;
		if (src1 & SLJIT_IMM)
			src1w = (src1w << 32) >> 32;
		if (src2 & SLJIT_IMM)
			src2w = (src2w << 32) >> 32;
		if (GET_FLAGS(op))
			inp_flags |= ALT_SIGN_EXT;
	}
#endif
	if (op & SLJIT_SET_O)
		FAIL_IF(push_inst(compiler, MTXER | S(ZERO_REG)));

	switch (GET_OPCODE(op)) {
	case SLJIT_ADD:
		if (!GET_FLAGS(op) && ((src1 | src2) & SLJIT_IMM)) {
			if (TEST_SL_IMM(src2, src2w)) {
				compiler->imm = src2w & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM1, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_SL_IMM(src1, src1w)) {
				compiler->imm = src1w & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM1, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
			if (TEST_SH_IMM(src2, src2w)) {
				compiler->imm = (src2w >> 16) & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM2, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_SH_IMM(src1, src1w)) {
				compiler->imm = (src1w >> 16) & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM2, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
			/* Range between -1 and -32768 is covered above. */
			if (TEST_ADD_IMM(src2, src2w)) {
				compiler->imm = src2w & 0xffffffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM4, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_ADD_IMM(src1, src1w)) {
				compiler->imm = src1w & 0xffffffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM4, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
		}
		if (!(GET_FLAGS(op) & (SLJIT_SET_E | SLJIT_SET_O))) {
			if (TEST_SL_IMM(src2, src2w)) {
				compiler->imm = src2w & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM3, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_SL_IMM(src1, src1w)) {
				compiler->imm = src1w & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM3, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
		}
		return emit_op(compiler, SLJIT_ADD, inp_flags, dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_ADDC:
		return emit_op(compiler, SLJIT_ADDC, inp_flags | (!(op & SLJIT_KEEP_FLAGS) ? 0 : ALT_FORM1), dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_SUB:
		if (!GET_FLAGS(op) && ((src1 | src2) & SLJIT_IMM)) {
			if (TEST_SL_IMM(src2, -src2w)) {
				compiler->imm = (-src2w) & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM1, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_SL_IMM(src1, src1w)) {
				compiler->imm = src1w & 0xffff;
				return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM1, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
			if (TEST_SH_IMM(src2, -src2w)) {
				compiler->imm = ((-src2w) >> 16) & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM2, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			/* Range between -1 and -32768 is covered above. */
			if (TEST_ADD_IMM(src2, -src2w)) {
				compiler->imm = -src2w & 0xffffffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM4, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
		}
		if (dst == SLJIT_UNUSED && (op & (SLJIT_SET_E | SLJIT_SET_S | SLJIT_SET_U)) && !(op & (SLJIT_SET_O | SLJIT_SET_C))) {
			if (!(op & SLJIT_SET_U)) {
				/* We know ALT_SIGN_EXT is set if it is an SLJIT_INT_OP on 64 bit systems. */
				if (TEST_SL_IMM(src2, src2w)) {
					compiler->imm = src2w & 0xffff;
					return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM2, dst, dstw, src1, src1w, TMP_REG2, 0);
				}
				if (GET_FLAGS(op) == SLJIT_SET_E && TEST_SL_IMM(src1, src1w)) {
					compiler->imm = src1w & 0xffff;
					return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM2, dst, dstw, src2, src2w, TMP_REG2, 0);
				}
			}
			if (!(op & (SLJIT_SET_E | SLJIT_SET_S))) {
				/* We know ALT_SIGN_EXT is set if it is an SLJIT_INT_OP on 64 bit systems. */
				if (TEST_UL_IMM(src2, src2w)) {
					compiler->imm = src2w & 0xffff;
					return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM3, dst, dstw, src1, src1w, TMP_REG2, 0);
				}
				return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM4, dst, dstw, src1, src1w, src2, src2w);
			}
			if ((src2 & SLJIT_IMM) && src2w >= 0 && src2w <= 0x7fff) {
				compiler->imm = src2w;
				return emit_op(compiler, SLJIT_SUB, inp_flags | ALT_FORM2 | ALT_FORM3, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			return emit_op(compiler, SLJIT_SUB, inp_flags | ((op & SLJIT_SET_U) ? ALT_FORM4 : 0) | ((op & (SLJIT_SET_E | SLJIT_SET_S)) ? ALT_FORM5 : 0), dst, dstw, src1, src1w, src2, src2w);
		}
		if (!(op & (SLJIT_SET_E | SLJIT_SET_S | SLJIT_SET_U | SLJIT_SET_O))) {
			if (TEST_SL_IMM(src2, -src2w)) {
				compiler->imm = (-src2w) & 0xffff;
				return emit_op(compiler, SLJIT_ADD, inp_flags | ALT_FORM3, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
		}
		/* We know ALT_SIGN_EXT is set if it is an SLJIT_INT_OP on 64 bit systems. */
		return emit_op(compiler, SLJIT_SUB, inp_flags | (!(op & SLJIT_SET_U) ? 0 : ALT_FORM6), dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_SUBC:
		return emit_op(compiler, SLJIT_SUBC, inp_flags | (!(op & SLJIT_KEEP_FLAGS) ? 0 : ALT_FORM1), dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_MUL:
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		if (op & SLJIT_INT_OP)
			inp_flags |= ALT_FORM2;
#endif
		if (!GET_FLAGS(op)) {
			if (TEST_SL_IMM(src2, src2w)) {
				compiler->imm = src2w & 0xffff;
				return emit_op(compiler, SLJIT_MUL, inp_flags | ALT_FORM1, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_SL_IMM(src1, src1w)) {
				compiler->imm = src1w & 0xffff;
				return emit_op(compiler, SLJIT_MUL, inp_flags | ALT_FORM1, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
		}
		return emit_op(compiler, SLJIT_MUL, inp_flags, dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_AND:
	case SLJIT_OR:
	case SLJIT_XOR:
		/* Commutative unsigned operations. */
		if (!GET_FLAGS(op) || GET_OPCODE(op) == SLJIT_AND) {
			if (TEST_UL_IMM(src2, src2w)) {
				compiler->imm = src2w;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM1, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_UL_IMM(src1, src1w)) {
				compiler->imm = src1w;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM1, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
			if (TEST_UH_IMM(src2, src2w)) {
				compiler->imm = (src2w >> 16) & 0xffff;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM2, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_UH_IMM(src1, src1w)) {
				compiler->imm = (src1w >> 16) & 0xffff;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM2, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
		}
		if (!GET_FLAGS(op) && GET_OPCODE(op) != SLJIT_AND) {
			if (TEST_UI_IMM(src2, src2w)) {
				compiler->imm = src2w;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM3, dst, dstw, src1, src1w, TMP_REG2, 0);
			}
			if (TEST_UI_IMM(src1, src1w)) {
				compiler->imm = src1w;
				return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM3, dst, dstw, src2, src2w, TMP_REG2, 0);
			}
		}
		return emit_op(compiler, GET_OPCODE(op), inp_flags, dst, dstw, src1, src1w, src2, src2w);

	case SLJIT_SHL:
	case SLJIT_LSHR:
	case SLJIT_ASHR:
#if (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
		if (op & SLJIT_INT_OP)
			inp_flags |= ALT_FORM2;
#endif
		if (src2 & SLJIT_IMM) {
			compiler->imm = src2w;
			return emit_op(compiler, GET_OPCODE(op), inp_flags | ALT_FORM1, dst, dstw, src1, src1w, TMP_REG2, 0);
		}
		return emit_op(compiler, GET_OPCODE(op), inp_flags, dst, dstw, src1, src1w, src2, src2w);
	}

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_get_register_index(int reg)
{
	check_sljit_get_register_index(reg);
	return reg_map[reg];
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op_custom(struct sljit_compiler *compiler,
	void *instruction, int size)
{
	CHECK_ERROR();
	check_sljit_emit_op_custom(compiler, instruction, size);
	SLJIT_ASSERT(size == 4);

	return push_inst(compiler, *(sljit_ins*)instruction);
}

/* --------------------------------------------------------------------- */
/*  Floating point operators                                             */
/* --------------------------------------------------------------------- */

SLJIT_API_FUNC_ATTRIBUTE int sljit_is_fpu_available(void)
{
	/* Always available. */
	return 1;
}

static int emit_fpu_data_transfer(struct sljit_compiler *compiler, int fpu_reg, int load, int arg, sljit_w argw)
{
	SLJIT_ASSERT(arg & SLJIT_MEM);

	/* Fast loads and stores. */
	if (!(arg & 0xf0)) {
		/* Both for (arg & 0xf) == SLJIT_UNUSED and (arg & 0xf) != SLJIT_UNUSED. */
		if (argw <= SIMM_MAX && argw >= SIMM_MIN)
			return push_inst(compiler, (load ? LFD : STFD) | FD(fpu_reg) | A(arg & 0xf) | IMM(argw));
	}

	if (arg & 0xf0) {
		argw &= 0x3;
		if (argw) {
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
			FAIL_IF(push_inst(compiler, RLWINM | S((arg >> 4) & 0xf) | A(TMP_REG2) | (argw << 11) | ((31 - argw) << 1)));
#else
			FAIL_IF(push_inst(compiler, RLDI(TMP_REG2, (arg >> 4) & 0xf, argw, 63 - argw, 1)));
#endif
			return push_inst(compiler, (load ? LFDX : STFDX) | FD(fpu_reg) | A(arg & 0xf) | B(TMP_REG2));
		}
		return push_inst(compiler, (load ? LFDX : STFDX) | FD(fpu_reg) | A(arg & 0xf) | B((arg >> 4) & 0xf));
	}

	/* Use cache. */
	if (compiler->cache_arg == arg && argw - compiler->cache_argw <= SIMM_MAX && argw - compiler->cache_argw >= SIMM_MIN)
		return push_inst(compiler, (load ? LFD : STFD) | FD(fpu_reg) | A(TMP_REG3) | IMM(argw - compiler->cache_argw));

	/* Put value to cache. */
	compiler->cache_arg = arg;
	compiler->cache_argw = argw;

	FAIL_IF(load_immediate(compiler, TMP_REG3, argw));
	if (!(arg & 0xf))
		return push_inst(compiler, (load ? LFDX : STFDX) | FD(fpu_reg) | A(0) | B(TMP_REG3));
	return push_inst(compiler, (load ? LFDUX : STFDUX) | FD(fpu_reg) | A(TMP_REG3) | B(arg & 0xf));
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fop1(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src, sljit_w srcw)
{
	int dst_fr;

	CHECK_ERROR();
	check_sljit_emit_fop1(compiler, op, dst, dstw, src, srcw);

	compiler->cache_arg = 0;
	compiler->cache_argw = 0;

	if (GET_OPCODE(op) == SLJIT_FCMP) {
		if (dst > SLJIT_FLOAT_REG4) {
			FAIL_IF(emit_fpu_data_transfer(compiler, TMP_FREG1, 1, dst, dstw));
			dst = TMP_FREG1;
		}
		if (src > SLJIT_FLOAT_REG4) {
			FAIL_IF(emit_fpu_data_transfer(compiler, TMP_FREG2, 1, src, srcw));
			src = TMP_FREG2;
		}
		return push_inst(compiler, FCMPU | CRD(4) | FA(dst) | FB(src));
	}

	dst_fr = (dst > SLJIT_FLOAT_REG4) ? TMP_FREG1 : dst;

	if (src > SLJIT_FLOAT_REG4) {
		FAIL_IF(emit_fpu_data_transfer(compiler, dst_fr, 1, src, srcw));
		src = dst_fr;
	}

	switch (op) {
		case SLJIT_FMOV:
			if (src != dst_fr && dst_fr != TMP_FREG1)
				FAIL_IF(push_inst(compiler, FMR | FD(dst_fr) | FB(src)));
			break;
		case SLJIT_FNEG:
			FAIL_IF(push_inst(compiler, FNEG | FD(dst_fr) | FB(src)));
			break;
		case SLJIT_FABS:
			FAIL_IF(push_inst(compiler, FABS | FD(dst_fr) | FB(src)));
			break;
	}

	if (dst_fr == TMP_FREG1)
		FAIL_IF(emit_fpu_data_transfer(compiler, src, 0, dst, dstw));

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fop2(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w)
{
	int dst_fr;

	CHECK_ERROR();
	check_sljit_emit_fop2(compiler, op, dst, dstw, src1, src1w, src2, src2w);

	compiler->cache_arg = 0;
	compiler->cache_argw = 0;

	dst_fr = (dst > SLJIT_FLOAT_REG4) ? TMP_FREG1 : dst;

	if (src2 > SLJIT_FLOAT_REG4) {
		FAIL_IF(emit_fpu_data_transfer(compiler, TMP_FREG2, 1, src2, src2w));
		src2 = TMP_FREG2;
	}

	if (src1 > SLJIT_FLOAT_REG4) {
		FAIL_IF(emit_fpu_data_transfer(compiler, TMP_FREG1, 1, src1, src1w));
		src1 = TMP_FREG1;
	}

	switch (op) {
	case SLJIT_FADD:
		FAIL_IF(push_inst(compiler, FADD | FD(dst_fr) | FA(src1) | FB(src2)));
		break;

	case SLJIT_FSUB:
		FAIL_IF(push_inst(compiler, FSUB | FD(dst_fr) | FA(src1) | FB(src2)));
		break;

	case SLJIT_FMUL:
		FAIL_IF(push_inst(compiler, FMUL | FD(dst_fr) | FA(src1) | FC(src2) /* FMUL use FC as src2 */));
		break;

	case SLJIT_FDIV:
		FAIL_IF(push_inst(compiler, FDIV | FD(dst_fr) | FA(src1) | FB(src2)));
		break;
	}

	if (dst_fr == TMP_FREG1)
		FAIL_IF(emit_fpu_data_transfer(compiler, TMP_FREG1, 0, dst, dstw));

	return SLJIT_SUCCESS;
}

/* --------------------------------------------------------------------- */
/*  Other instructions                                                   */
/* --------------------------------------------------------------------- */

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_enter(struct sljit_compiler *compiler, int dst, sljit_w dstw, int args, int temporaries, int saveds, int local_size)
{
	CHECK_ERROR();
	check_sljit_emit_fast_enter(compiler, dst, dstw, args, temporaries, saveds, local_size);

	compiler->temporaries = temporaries;
	compiler->saveds = saveds;

	compiler->has_locals = local_size > 0;
#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32)
	compiler->local_size = (2 + saveds + 2) * sizeof(sljit_w) + local_size;
#else
	compiler->local_size = (2 + saveds + 7 + 8) * sizeof(sljit_w) + local_size;
#endif
	compiler->local_size = (compiler->local_size + 15) & ~0xf;

	if (dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_NO_REGISTERS)
		return push_inst(compiler, MFLR | D(dst));
	else if (dst & SLJIT_MEM) {
		FAIL_IF(push_inst(compiler, MFLR | D(TMP_REG2)));
		return emit_op(compiler, SLJIT_MOV, WORD_DATA, dst, dstw, TMP_REG1, 0, TMP_REG2, 0);
	}

	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_return(struct sljit_compiler *compiler, int src, sljit_w srcw)
{
	CHECK_ERROR();
	check_sljit_emit_fast_return(compiler, src, srcw);

	if (src >= SLJIT_TEMPORARY_REG1 && src <= SLJIT_NO_REGISTERS)
		FAIL_IF(push_inst(compiler, MTLR | S(src)));
	else {
		if (src & SLJIT_MEM)
			FAIL_IF(emit_op(compiler, SLJIT_MOV, WORD_DATA, TMP_REG2, 0, TMP_REG1, 0, src, srcw));
		else if (src & SLJIT_IMM)
			FAIL_IF(load_immediate(compiler, TMP_REG2, srcw));
		FAIL_IF(push_inst(compiler, MTLR | S(TMP_REG2)));
	}
	return push_inst(compiler, BLR);
}

/* --------------------------------------------------------------------- */
/*  Conditional instructions                                             */
/* --------------------------------------------------------------------- */

SLJIT_API_FUNC_ATTRIBUTE struct sljit_label* sljit_emit_label(struct sljit_compiler *compiler)
{
	struct sljit_label *label;

	CHECK_ERROR_PTR();
	check_sljit_emit_label(compiler);

	if (compiler->last_label && compiler->last_label->size == compiler->size)
		return compiler->last_label;

	label = (struct sljit_label*)ensure_abuf(compiler, sizeof(struct sljit_label));
	PTR_FAIL_IF(!label);
	set_label(label, compiler);
	return label;
}

static sljit_ins get_bo_bi_flags(struct sljit_compiler *compiler, int type)
{
	switch (type) {
	case SLJIT_C_EQUAL:
		return (12 << 21) | (2 << 16);

	case SLJIT_C_NOT_EQUAL:
		return (4 << 21) | (2 << 16);

	case SLJIT_C_LESS:
	case SLJIT_C_FLOAT_LESS:
		return (12 << 21) | ((4 + 0) << 16);

	case SLJIT_C_GREATER_EQUAL:
	case SLJIT_C_FLOAT_GREATER_EQUAL:
		return (4 << 21) | ((4 + 0) << 16);

	case SLJIT_C_GREATER:
	case SLJIT_C_FLOAT_GREATER:
		return (12 << 21) | ((4 + 1) << 16);

	case SLJIT_C_LESS_EQUAL:
	case SLJIT_C_FLOAT_LESS_EQUAL:
		return (4 << 21) | ((4 + 1) << 16);

	case SLJIT_C_SIG_LESS:
		return (12 << 21) | (0 << 16);

	case SLJIT_C_SIG_GREATER_EQUAL:
		return (4 << 21) | (0 << 16);

	case SLJIT_C_SIG_GREATER:
		return (12 << 21) | (1 << 16);

	case SLJIT_C_SIG_LESS_EQUAL:
		return (4 << 21) | (1 << 16);

	case SLJIT_C_OVERFLOW:
	case SLJIT_C_MUL_OVERFLOW:
		return (12 << 21) | (3 << 16);

	case SLJIT_C_NOT_OVERFLOW:
	case SLJIT_C_MUL_NOT_OVERFLOW:
		return (4 << 21) | (3 << 16);

	case SLJIT_C_FLOAT_EQUAL:
		return (12 << 21) | ((4 + 2) << 16);

	case SLJIT_C_FLOAT_NOT_EQUAL:
		return (4 << 21) | ((4 + 2) << 16);

	case SLJIT_C_FLOAT_NAN:
		return (12 << 21) | ((4 + 3) << 16);

	case SLJIT_C_FLOAT_NOT_NAN:
		return (4 << 21) | ((4 + 3) << 16);

	default:
		SLJIT_ASSERT(type >= SLJIT_JUMP && type <= SLJIT_CALL3);
		return (20 << 21);
	}
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_jump(struct sljit_compiler *compiler, int type)
{
	struct sljit_jump *jump;
	sljit_ins bo_bi_flags;

	CHECK_ERROR_PTR();
	check_sljit_emit_jump(compiler, type);

	bo_bi_flags = get_bo_bi_flags(compiler, type & 0xff);
	if (!bo_bi_flags)
		return NULL;

	jump = (struct sljit_jump*)ensure_abuf(compiler, sizeof(struct sljit_jump));
	PTR_FAIL_IF(!jump);
	set_jump(jump, compiler, type & SLJIT_REWRITABLE_JUMP);
	type &= 0xff;

	/* In PPC, we don't need to touch the arguments. */
	if (type >= SLJIT_JUMP)
		jump->flags |= UNCOND_B;

	PTR_FAIL_IF(emit_const(compiler, TMP_REG1, 0));
	PTR_FAIL_IF(push_inst(compiler, MTCTR | S(TMP_REG1)));
	jump->addr = compiler->size;
	PTR_FAIL_IF(push_inst(compiler, BCCTR | bo_bi_flags | (type >= SLJIT_FAST_CALL ? 1 : 0)));
	return jump;
}

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_ijump(struct sljit_compiler *compiler, int type, int src, sljit_w srcw)
{
	sljit_ins bo_bi_flags;
	struct sljit_jump *jump = NULL;
	int src_r;

	CHECK_ERROR();
	check_sljit_emit_ijump(compiler, type, src, srcw);

	bo_bi_flags = get_bo_bi_flags(compiler, type);
	FAIL_IF(!bo_bi_flags);

	if (src >= SLJIT_TEMPORARY_REG1 && src <= SLJIT_NO_REGISTERS)
		src_r = src;
	else if (src & SLJIT_IMM) {
		jump = (struct sljit_jump*)ensure_abuf(compiler, sizeof(struct sljit_jump));
		FAIL_IF(!jump);
		set_jump(jump, compiler, JUMP_ADDR | UNCOND_B);
		jump->u.target = srcw;

		FAIL_IF(emit_const(compiler, TMP_REG2, 0));
		src_r = TMP_REG2;
	}
	else {
		FAIL_IF(emit_op(compiler, SLJIT_MOV, WORD_DATA, TMP_REG2, 0, TMP_REG1, 0, src, srcw));
		src_r = TMP_REG2;
	}

	FAIL_IF(push_inst(compiler, MTCTR | S(src_r)));
	if (jump)
		jump->addr = compiler->size;
	return push_inst(compiler, BCCTR | bo_bi_flags | (type >= SLJIT_FAST_CALL ? 1 : 0));
}

/* Get a bit from CR, all other bits are zeroed. */
#define GET_CR_BIT(bit, dst) \
	FAIL_IF(push_inst(compiler, MFCR | D(dst))); \
	FAIL_IF(push_inst(compiler, RLWINM | S(dst) | A(dst) | ((1 + (bit)) << 11) | (31 << 6) | (31 << 1)));

#define INVERT_BIT(dst) \
	FAIL_IF(push_inst(compiler, XORI | S(dst) | A(dst) | 0x1));

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_cond_value(struct sljit_compiler *compiler, int op, int dst, sljit_w dstw, int type)
{
	int reg;

	CHECK_ERROR();
	check_sljit_emit_cond_value(compiler, op, dst, dstw, type);

	if (dst == SLJIT_UNUSED)
		return SLJIT_SUCCESS;

	reg = (op == SLJIT_MOV && dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_NO_REGISTERS) ? dst : TMP_REG2;

	switch (type) {
	case SLJIT_C_EQUAL:
		GET_CR_BIT(2, reg);
		break;

	case SLJIT_C_NOT_EQUAL:
		GET_CR_BIT(2, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_LESS:
	case SLJIT_C_FLOAT_LESS:
		GET_CR_BIT(4 + 0, reg);
		break;

	case SLJIT_C_GREATER_EQUAL:
	case SLJIT_C_FLOAT_GREATER_EQUAL:
		GET_CR_BIT(4 + 0, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_GREATER:
	case SLJIT_C_FLOAT_GREATER:
		GET_CR_BIT(4 + 1, reg);
		break;

	case SLJIT_C_LESS_EQUAL:
	case SLJIT_C_FLOAT_LESS_EQUAL:
		GET_CR_BIT(4 + 1, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_SIG_LESS:
		GET_CR_BIT(0, reg);
		break;

	case SLJIT_C_SIG_GREATER_EQUAL:
		GET_CR_BIT(0, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_SIG_GREATER:
		GET_CR_BIT(1, reg);
		break;

	case SLJIT_C_SIG_LESS_EQUAL:
		GET_CR_BIT(1, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_OVERFLOW:
	case SLJIT_C_MUL_OVERFLOW:
		GET_CR_BIT(3, reg);
		break;

	case SLJIT_C_NOT_OVERFLOW:
	case SLJIT_C_MUL_NOT_OVERFLOW:
		GET_CR_BIT(3, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_FLOAT_EQUAL:
		GET_CR_BIT(4 + 2, reg);
		break;

	case SLJIT_C_FLOAT_NOT_EQUAL:
		GET_CR_BIT(4 + 2, reg);
		INVERT_BIT(reg);
		break;

	case SLJIT_C_FLOAT_NAN:
		GET_CR_BIT(4 + 3, reg);
		break;

	case SLJIT_C_FLOAT_NOT_NAN:
		GET_CR_BIT(4 + 3, reg);
		INVERT_BIT(reg);
		break;

	default:
		SLJIT_ASSERT_STOP();
		break;
	}

	if (GET_OPCODE(op) == SLJIT_OR)
		return emit_op(compiler, GET_OPCODE(op), GET_FLAGS(op) ? ALT_SET_FLAGS : 0, dst, dstw, dst, dstw, TMP_REG2, 0);

	if (reg == TMP_REG2)
		return emit_op(compiler, SLJIT_MOV, WORD_DATA, dst, dstw, TMP_REG1, 0, TMP_REG2, 0);
	return SLJIT_SUCCESS;
}

SLJIT_API_FUNC_ATTRIBUTE struct sljit_const* sljit_emit_const(struct sljit_compiler *compiler, int dst, sljit_w dstw, sljit_w init_value)
{
	struct sljit_const *const_;
	int reg;

	CHECK_ERROR_PTR();
	check_sljit_emit_const(compiler, dst, dstw, init_value);

	const_ = (struct sljit_const*)ensure_abuf(compiler, sizeof(struct sljit_const));
	PTR_FAIL_IF(!const_);
	set_const(const_, compiler);

	reg = (dst >= SLJIT_TEMPORARY_REG1 && dst <= SLJIT_NO_REGISTERS) ? dst : TMP_REG2;

	PTR_FAIL_IF(emit_const(compiler, reg, init_value));

	if (dst & SLJIT_MEM)
		PTR_FAIL_IF(emit_op(compiler, SLJIT_MOV, WORD_DATA, dst, dstw, TMP_REG1, 0, TMP_REG2, 0));
	return const_;
}
