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

#ifndef _SLJIT_LIR_H_
#define _SLJIT_LIR_H_

/*
   ------------------------------------------------------------------------
    Stack-Less JIT compiler for multiple architectures (x86, ARM, PowerPC)
   ------------------------------------------------------------------------

   Short description
    Advantages:
      - The execution can be continued from any LIR instruction
        In other words, jump into and out of the code is safe
      - Both target of (conditional) jump and call instructions
        and constants can be dynamically modified during runtime
        - although it is not suggested to do it frequently
        - very effective to cache an important value once
      - A fixed stack space can be allocated for local variables
      - The compiler is thread-safe
    Disadvantages:
      - Limited number of registers (only 6+4 integer registers, max 3+2
        temporary, max 3+2 saved and 4 floating point registers)
    In practice:
      - This approach is very effective for interpreters
        - One of the saved registers typically points to a stack interface
        - It can jump to any exception handler anytime (even for another
          function. It is safe for SLJIT.)
        - Fast paths can be modified during runtime reflecting the changes
          of the fastest execution path of the dynamic language
        - SLJIT supports complex memory addressing modes
        - mainly position independent code
      - Optimizations (perhaps later)
        - Only for basic blocks (when no labels inserted between LIR instructions)

    For valgrind users:
      - pass --smc-check=all argument to valgrind, since JIT is a "self-modifying code"
*/

#if !(defined SLJIT_NO_DEFAULT_CONFIG && SLJIT_NO_DEFAULT_CONFIG)
#include "sljitConfig.h"
#endif

/* The following header file defines useful macros for fine tuning
sljit based code generators. They are listed in the begining
of sljitConfigInternal.h */

#include "sljitConfigInternal.h"

/* --------------------------------------------------------------------- */
/*  Error codes                                                          */
/* --------------------------------------------------------------------- */

/* Indicates no error. */
#define SLJIT_SUCCESS			0
/* After the call of sljit_generate_code(), the error code of the compiler
   is set to this value to avoid future sljit calls (in debug mode at least).
   The complier should be freed after sljit_generate_code(). */
#define SLJIT_ERR_COMPILED		1
/* Cannot allocate non executable memory. */
#define SLJIT_ERR_ALLOC_FAILED		2
/* Cannot allocate executable memory.
   Only for sljit_generate_code() */
#define SLJIT_ERR_EX_ALLOC_FAILED	3
/* return value for SLJIT_CONFIG_UNSUPPORTED empty architecture. */
#define SLJIT_ERR_UNSUPPORTED		4

/* --------------------------------------------------------------------- */
/*  Registers                                                            */
/* --------------------------------------------------------------------- */

#define SLJIT_UNUSED		0

/* Temporary (scratch) registers may not preserve their values across function calls. */
#define SLJIT_TEMPORARY_REG1	1
#define SLJIT_TEMPORARY_REG2	2
#define SLJIT_TEMPORARY_REG3	3
/* Note: Extra Registers cannot be used for memory addressing. */
/* Note: on x86-32, these registers are emulated (using stack loads & stores). */
#define SLJIT_TEMPORARY_EREG1	4
#define SLJIT_TEMPORARY_EREG2	5

/* Saved registers whose preserve their values across function calls. */
#define SLJIT_SAVED_REG1	6
#define SLJIT_SAVED_REG2	7
#define SLJIT_SAVED_REG3	8
/* Note: Extra Registers cannot be used for memory addressing. */
/* Note: on x86-32, these registers are emulated (using stack loads & stores). */
#define SLJIT_SAVED_EREG1	9
#define SLJIT_SAVED_EREG2	10

/* Read-only register (cannot be the destination of an operation). */
/* Note: SLJIT_MEM2( ... , SLJIT_LOCALS_REG) is not supported (x86 limitation). */
/* Note: SLJIT_LOCALS_REG is not necessary the real stack pointer. See sljit_emit_enter. */
#define SLJIT_LOCALS_REG	11

/* Number of registers. */
#define SLJIT_NO_TMP_REGISTERS	5
#define SLJIT_NO_GEN_REGISTERS	5
#define SLJIT_NO_REGISTERS	11

/* Return with machine word. */

#define SLJIT_RETURN_REG	SLJIT_TEMPORARY_REG1

/* x86 prefers specific registers for special purposes. In case of shift
   by register it supports only SLJIT_TEMPORARY_REG3 for shift argument
   (which is the src2 argument of sljit_emit_op2). If another register is
   used, sljit must exchange data between registers which cause a minor
   slowdown. Other architectures has no such limitation. */

#define SLJIT_PREF_SHIFT_REG	SLJIT_TEMPORARY_REG3

/* --------------------------------------------------------------------- */
/*  Floating point registers                                             */
/* --------------------------------------------------------------------- */

/* Note: SLJIT_UNUSED as destination is not valid for floating point
     operations, since they cannot be used for setting flags. */

/* Floating point operations are performed on double precision values. */

#define SLJIT_FLOAT_REG1	1
#define SLJIT_FLOAT_REG2	2
#define SLJIT_FLOAT_REG3	3
#define SLJIT_FLOAT_REG4	4

/* --------------------------------------------------------------------- */
/*  Main structures and functions                                        */
/* --------------------------------------------------------------------- */

struct sljit_memory_fragment {
	struct sljit_memory_fragment *next;
	sljit_uw used_size;
	sljit_ub memory[1];
};

struct sljit_label {
	struct sljit_label *next;
	sljit_uw addr;
	/* The maximum size difference. */
	sljit_uw size;
};

struct sljit_jump {
	struct sljit_jump *next;
	sljit_uw addr;
	sljit_w flags;
	union {
		sljit_uw target;
		struct sljit_label* label;
	} u;
};

struct sljit_const {
	struct sljit_const *next;
	sljit_uw addr;
};

struct sljit_compiler {
	int error;

	struct sljit_label *labels;
	struct sljit_jump *jumps;
	struct sljit_const *consts;
	struct sljit_label *last_label;
	struct sljit_jump *last_jump;
	struct sljit_const *last_const;

	struct sljit_memory_fragment *buf;
	struct sljit_memory_fragment *abuf;

	/* Used local registers. */
	int temporaries;
	/* Used saved registers. */
	int saveds;
	/* Local stack size. */
	int local_size;
	/* Code size. */
	sljit_uw size;
	/* For statistical purposes. */
	sljit_uw executable_size;

#if (defined SLJIT_CONFIG_X86_32 && SLJIT_CONFIG_X86_32)
	int args;
	int temporaries_start;
	int saveds_start;
#endif

#if (defined SLJIT_CONFIG_X86_64 && SLJIT_CONFIG_X86_64)
	int mode32;
#ifdef _WIN64
	int has_locals;
#endif
#endif

#if (defined SLJIT_CONFIG_X86_32 && SLJIT_CONFIG_X86_32) || (defined SLJIT_CONFIG_X86_64 && SLJIT_CONFIG_X86_64)
	int flags_saved;
#endif

#if (defined SLJIT_CONFIG_ARM_V5 && SLJIT_CONFIG_ARM_V5)
	/* Constant pool handling. */
	sljit_uw *cpool;
	sljit_ub *cpool_unique;
	sljit_uw cpool_diff;
	sljit_uw cpool_fill;
	/* Other members. */
	/* Contains pointer, "ldr pc, [...]" pairs. */
	sljit_uw patches;
#endif

#if (defined SLJIT_CONFIG_ARM_V5 && SLJIT_CONFIG_ARM_V5) || (defined SLJIT_CONFIG_ARM_V7 && SLJIT_CONFIG_ARM_V7)
	/* Temporary fields. */
	sljit_uw shift_imm;
	int cache_arg;
	sljit_w cache_argw;
#endif

#if (defined SLJIT_CONFIG_ARM_THUMB2 && SLJIT_CONFIG_ARM_THUMB2)
	int cache_arg;
	sljit_w cache_argw;
#endif

#if (defined SLJIT_CONFIG_PPC_32 && SLJIT_CONFIG_PPC_32) || (defined SLJIT_CONFIG_PPC_64 && SLJIT_CONFIG_PPC_64)
	int has_locals;
	sljit_w imm;
	int cache_arg;
	sljit_w cache_argw;
#endif

#if (defined SLJIT_CONFIG_MIPS_32 && SLJIT_CONFIG_MIPS_32)
	int has_locals;
	int delay_slot;
	int cache_arg;
	sljit_w cache_argw;
#endif

#if (defined SLJIT_VERBOSE && SLJIT_VERBOSE)
	FILE* verbose;
#endif

#if (defined SLJIT_VERBOSE && SLJIT_VERBOSE) || (defined SLJIT_DEBUG && SLJIT_DEBUG)
	int skip_checks;
#endif
};

/* --------------------------------------------------------------------- */
/*  Main functions                                                       */
/* --------------------------------------------------------------------- */

/* Creates an sljit compiler.
   Returns NULL if failed. */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_compiler* sljit_create_compiler(void);
/* Free everything except the codes. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_free_compiler(struct sljit_compiler *compiler);

static SLJIT_INLINE int sljit_get_compiler_error(struct sljit_compiler *compiler) { return compiler->error; }

/*
   Allocate a small amount of memory. The size must be <= 64 bytes on 32 bit,
   and <= 128 bytes on 64 bit architectures. The memory area is owned by the compiler,
   and freed by sljit_free_compiler. The returned pointer is sizeof(sljit_w) aligned.
   Excellent for allocating small blocks during the compiling, and no need to worry
   about freeing them. The size is enough to contain at most 16 pointers.
   If the size is outside of the range, the function will return with NULL,
   but this return value does not indicate that there is no more memory (does
   not set the compiler to out-of-memory status).
*/
SLJIT_API_FUNC_ATTRIBUTE void* sljit_alloc_memory(struct sljit_compiler *compiler, int size);

#if (defined SLJIT_VERBOSE && SLJIT_VERBOSE)
/* Passing NULL disables verbose. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_compiler_verbose(struct sljit_compiler *compiler, FILE* verbose);
#endif

SLJIT_API_FUNC_ATTRIBUTE void* sljit_generate_code(struct sljit_compiler *compiler);
SLJIT_API_FUNC_ATTRIBUTE void sljit_free_code(void* code);

/*
   After the code generation we can retrieve the allocated executable memory size,
   although this area may not be fully filled with instructions depending on some
   optimizations. This function is useful only for statistical purposes.

   Before a successful code generation, this function returns with 0.
*/
static SLJIT_INLINE sljit_uw sljit_get_generated_code_size(struct sljit_compiler *compiler) { return compiler->executable_size; }

/* Instruction generation. Returns with error code. */

/*
   The executable code is basically a function call from the viewpoint of
   the C language. The function calls must obey to the ABI (Application
   Binary Interface) of the platform, which specify the purpose of machine
   registers and stack handling among other things. The sljit_emit_enter
   function emits the necessary instructions for setting up a new context
   for the executable code and moves function arguments to the saved
   registers. The number of arguments are specified in the "args"
   parameter and the first argument goes to SLJIT_SAVED_REG1, the second
   goes to SLJIT_SAVED_REG2 and so on. The number of temporary and
   saved registers are passed in "temporaries" and "saveds" arguments
   respectively. Since the saved registers contains the arguments,
   "args" must be less or equal than "saveds". The sljit_emit_enter
   is also capable of allocating a stack space for local variables. The
   "local_size" argument contains the size in bytes of this local area
   and its staring address is stored in SLJIT_LOCALS_REG. However
   the SLJIT_LOCALS_REG is not necessary the machine stack pointer.
   The memory bytes between SLJIT_LOCALS_REG (inclusive) and
   SLJIT_LOCALS_REG + local_size (exclusive) can be modified freely
   until the function returns. The stack space is uninitialized.

   Note: every call of sljit_emit_enter and sljit_set_context overwrites
         the previous context. */

#define SLJIT_MAX_LOCAL_SIZE	65536

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_enter(struct sljit_compiler *compiler,
	int args, int temporaries, int saveds, int local_size);

/* The machine code has a context (which contains the local stack space size,
   number of used registers, etc.) which initialized by sljit_emit_enter. Several
   functions (like sljit_emit_return) requres this context to be able to generate
   the appropriate code. However, some code fragments (like inline cache) may have
   no normal entry point so their context is unknown for the compiler. Using the
   function below we can specify thir context.

   Note: every call of sljit_emit_enter and sljit_set_context overwrites
         the previous context. */

/* Note: multiple calls of this function overwrites the previous call. */

SLJIT_API_FUNC_ATTRIBUTE void sljit_set_context(struct sljit_compiler *compiler,
	int args, int temporaries, int saveds, int local_size);

/* Return from machine code.  The op argument can be SLJIT_UNUSED which means the
   function does not return with anything or any opcode between SLJIT_MOV and
   SLJIT_MOV_SI (see sljit_emit_op1). As for src and srcw they must be 0 if op
   is SLJIT_UNUSED, otherwise see below the description about source and
   destination arguments. */
SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_return(struct sljit_compiler *compiler, int op,
	int src, sljit_w srcw);

/* Really fast calling method for utility functions inside sljit (see SLJIT_FAST_CALL).
   All registers and even the stack frame is passed to the callee. The return address is
   preserved in dst/dstw by sljit_emit_fast_enter, and sljit_emit_fast_return can
   use this as a return value later. */

/* Note: only for sljit specific, non ABI compilant calls. Fast, since only a few machine instructions
   are needed. Excellent for small uility functions, where saving registers and setting up
   a new stack frame would cost too much performance. However, it is still possible to return
   to the address of the caller (or anywhere else). */

/* Note: flags are not changed (unlike sljit_emit_enter / sljit_emit_return). */

/* Note: although sljit_emit_fast_return could be replaced by an ijump, it is not suggested,
   since many architectures do clever branch prediction on call / return instruction pairs. */

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_enter(struct sljit_compiler *compiler, int dst, sljit_w dstw, int args, int temporaries, int saveds, int local_size);
SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fast_return(struct sljit_compiler *compiler, int src, sljit_w srcw);

/*
   Source and destination values for arithmetical instructions
    imm              - a simple immediate value (cannot be used as a destination)
    reg              - any of the registers (immediate argument must be 0)
    [imm]            - absolute immediate memory address
    [reg+imm]        - indirect memory address
    [reg+(reg<<imm)] - indirect indexed memory address (shift must be between 0 and 3)
                       useful for (byte, half, int, sljit_w) array access
                       (fully supported by both x86 and ARM architectures, and cheap operation on others)
*/

/*
   IMPORATNT NOTE: memory access MUST be naturally aligned except
                   SLJIT_UNALIGNED macro is defined and its value is 1.

     length | alignment
   ---------+-----------
     byte   | 1 byte (not aligned)
     half   | 2 byte (real_address & 0x1 == 0)
     int    | 4 byte (real_address & 0x3 == 0)
    sljit_w | 4 byte if SLJIT_32BIT_ARCHITECTURE is defined and its value is 1
            | 8 byte if SLJIT_64BIT_ARCHITECTURE is defined and its value is 1

   Note: different architectures have different addressing limitations
         Thus sljit may generate several instructions for other addressing modes
   x86:  all addressing modes supported, but write-back is not supported
         (requires an extra instruction). On x86-64 only 32 bit signed
         integers are supported by the architecture.
   arm:  [reg+imm] supported for small immediates (-4095 <= imm <= 4095
         or -255 <= imm <= 255 for loading signed bytes, any halfs or doubles)
         [reg+(reg<<imm)] are supported or requires only two instructions
         Write back is limited to small immediates on thumb2
   ppc:  [reg+imm], -65535 <= imm <= 65535. 64 bit moves requires immediates
         divisible by 4. [reg+reg] supported, write-back supported
         [reg+(reg<<imm)] (imm != 0) is cheap (requires two instructions)
*/

/* Register output: simply the name of the register.
   For destination, you can use SLJIT_UNUSED as well. */
#define SLJIT_MEM		0x100
#define SLJIT_MEM0()		(SLJIT_MEM)
#define SLJIT_MEM1(r1)		(SLJIT_MEM | (r1))
#define SLJIT_MEM2(r1, r2)	(SLJIT_MEM | (r1) | ((r2) << 4))
#define SLJIT_IMM		0x200

/* Set 32 bit operation mode (I) on 64 bit CPUs. The flag is totally ignored on
   32 bit CPUs. The arithmetic instruction uses only the lower 32 bit of the
   input register(s), and set the flags according to the 32 bit result. If the
   destination is a register, the higher 32 bit of the result is undefined.
   The addressing modes (SLJIT_MEM1/SLJIT_MEM2 macros) are unaffected by this flag. */
#define SLJIT_INT_OP		0x100

/* Common CPU status flags for all architectures (x86, ARM, PPC)
    - carry flag
    - overflow flag
    - zero flag
    - negative/positive flag (depends on arc)
   On mips, these flags are emulated by software. */

/* By default, the instructions may, or may not set the CPU status flags.
   Forcing to set or keep status flags can be done with the following flags: */

/* Note: sljit tries to emit the minimum number of instructions. Using these
   flags can increase them, so use them wisely to avoid unnecessary code generation. */

/* Set Equal (Zero) status flag (E). */
#define SLJIT_SET_E			0x0200
/* Set signed status flag (S). */
#define SLJIT_SET_S			0x0400
/* Set unsgined status flag (U). */
#define SLJIT_SET_U			0x0800
/* Set signed overflow flag (O). */
#define SLJIT_SET_O			0x1000
/* Set carry flag (C).
   Note: Kinda unsigned overflow, but behaves differently on various cpus. */
#define SLJIT_SET_C			0x2000
/* Do not modify the flags (K).
   Note: This flag cannot be combined with any other SLJIT_SET_* flag. */
#define SLJIT_KEEP_FLAGS		0x4000

/* Notes:
     - you cannot postpone conditional jump instructions except if noted that
       the instruction does not set flags (See: SLJIT_KEEP_FLAGS).
     - flag combinations: '|' means 'logical or'. */

/* Flags: - (never set any flags)
   Note: breakpoint instruction is not supported by all architectures (namely ppc)
         It falls back to SLJIT_NOP in those cases. */
#define SLJIT_BREAKPOINT		0
/* Flags: - (never set any flags)
   Note: may or may not cause an extra cycle wait
         it can even decrease the runtime in a few cases. */
#define SLJIT_NOP			1
/* Flags: may destroy flags
   Unsigned multiplication of SLJIT_TEMPORARY_REG1 and SLJIT_TEMPORARY_REG2.
   Result goes to SLJIT_TEMPORARY_REG2:SLJIT_TEMPORARY_REG1 (high:low) word */
#define SLJIT_UMUL			2
/* Flags: may destroy flags
   Signed multiplication of SLJIT_TEMPORARY_REG1 and SLJIT_TEMPORARY_REG2.
   Result goes to SLJIT_TEMPORARY_REG2:SLJIT_TEMPORARY_REG1 (high:low) word */
#define SLJIT_SMUL			3
/* Flags: I | may destroy flags
   Unsigned divide of the value in SLJIT_TEMPORARY_REG1 by the value in SLJIT_TEMPORARY_REG2.
   The result is placed in SLJIT_TEMPORARY_REG1 and the remainder goes to SLJIT_TEMPORARY_REG2.
   Note: if SLJIT_TEMPORARY_REG2 contains 0, the behaviour is undefined. */
#define SLJIT_UDIV			4
/* Flags: I | may destroy flags
   Signed divide of the value in SLJIT_TEMPORARY_REG1 by the value in SLJIT_TEMPORARY_REG2.
   The result is placed in SLJIT_TEMPORARY_REG1 and the remainder goes to SLJIT_TEMPORARY_REG2.
   Note: if SLJIT_TEMPORARY_REG2 contains 0, the behaviour is undefined. */
#define SLJIT_SDIV			5

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op0(struct sljit_compiler *compiler, int op);

/* Notes for MOV instructions:
   U = Mov with update (post form). If source or destination defined as SLJIT_MEM1(r1)
       or SLJIT_MEM2(r1, r2), r1 is increased by the sum of r2 and the constant argument
   UB = unsigned byte (8 bit)
   SB = signed byte (8 bit)
   UH = unsgined half (16 bit)
   SH = unsgined half (16 bit) */

/* Flags: - (never set any flags) */
#define SLJIT_MOV			6
/* Flags: - (never set any flags) */
#define SLJIT_MOV_UB			7
/* Flags: - (never set any flags) */
#define SLJIT_MOV_SB			8
/* Flags: - (never set any flags) */
#define SLJIT_MOV_UH			9
/* Flags: - (never set any flags) */
#define SLJIT_MOV_SH			10
/* Flags: - (never set any flags) */
#define SLJIT_MOV_UI			11
/* Flags: - (never set any flags) */
#define SLJIT_MOV_SI			12
/* Flags: - (never set any flags) */
#define SLJIT_MOVU			13
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_UB			14
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_SB			15
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_UH			16
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_SH			17
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_UI			18
/* Flags: - (never set any flags) */
#define SLJIT_MOVU_SI			19
/* Flags: I | E | K */
#define SLJIT_NOT			20
/* Flags: I | E | O | K */
#define SLJIT_NEG			21
/* Count leading zeroes
   Flags: I | E | K */
#define SLJIT_CLZ			22

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op1(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src, sljit_w srcw);

/* Flags: I | E | O | C | K */
#define SLJIT_ADD			23
/* Flags: I | C | K */
#define SLJIT_ADDC			24
/* Flags: I | E | S | U | O | C | K */
#define SLJIT_SUB			25
/* Flags: I | C | K */
#define SLJIT_SUBC			26
/* Note: integer mul
   Flags: I | O (see SLJIT_C_MUL_*) | K */
#define SLJIT_MUL			27
/* Flags: I | E | K */
#define SLJIT_AND			28
/* Flags: I | E | K */
#define SLJIT_OR			29
/* Flags: I | E | K */
#define SLJIT_XOR			30
/* Flags: I | E | K
   Let bit_length be the length of the shift operation: 32 or 64.
   If src2 is immediate, src2w is masked by (bit_length - 1).
   Otherwise, if the content of src2 is outside the range from 0
   to bit_length - 1, the operation is undefined. */
#define SLJIT_SHL			31
/* Flags: I | E | K
   Let bit_length be the length of the shift operation: 32 or 64.
   If src2 is immediate, src2w is masked by (bit_length - 1).
   Otherwise, if the content of src2 is outside the range from 0
   to bit_length - 1, the operation is undefined. */
#define SLJIT_LSHR			32
/* Flags: I | E | K
   Let bit_length be the length of the shift operation: 32 or 64.
   If src2 is immediate, src2w is masked by (bit_length - 1).
   Otherwise, if the content of src2 is outside the range from 0
   to bit_length - 1, the operation is undefined. */
#define SLJIT_ASHR			33

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op2(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w);

/* The following function is a helper function for sljit_emit_op_custom.
   It returns with the real machine register index of any SLJIT_TEMPORARY
   SLJIT_SAVED or SLJIT_LOCALS register.
   Note: it returns with -1 for virtual registers (all EREGs on x86-32).
   Note: register returned by SLJIT_LOCALS_REG is not necessary the real
         stack pointer register of the target architecture. */

SLJIT_API_FUNC_ATTRIBUTE int sljit_get_register_index(int reg);

/* Any instruction can be inserted into the instruction stream by
   sljit_emit_op_custom. It has a similar purpose as inline assembly.
   The size parameter must match to the instruction size of the target
   architecture:

         x86: 0 < size <= 15. The instruction argument can be byte aligned.
      Thumb2: if size == 2, the instruction argument must be 2 byte aligned.
              if size == 4, the instruction argument must be 4 byte aligned.
   Otherwise: size must be 4 and instruction argument must be 4 byte aligned. */

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_op_custom(struct sljit_compiler *compiler,
	void *instruction, int size);

/* Returns with non-zero if fpu is available. */

SLJIT_API_FUNC_ATTRIBUTE int sljit_is_fpu_available(void);

/* Note: dst is the left and src is the right operand for SLJIT_FCMP.
   Note: NaN check is always performed. If SLJIT_C_FLOAT_NAN is set,
         the comparison result is unpredictable.
   Flags: E | S (see SLJIT_C_FLOAT_*) */
#define SLJIT_FCMP			34
/* Flags: - (never set any flags) */
#define SLJIT_FMOV			35
/* Flags: - (never set any flags) */
#define SLJIT_FNEG			36
/* Flags: - (never set any flags) */
#define SLJIT_FABS			37

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fop1(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src, sljit_w srcw);

/* Flags: - (never set any flags) */
#define SLJIT_FADD			38
/* Flags: - (never set any flags) */
#define SLJIT_FSUB			39
/* Flags: - (never set any flags) */
#define SLJIT_FMUL			40
/* Flags: - (never set any flags) */
#define SLJIT_FDIV			41

SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_fop2(struct sljit_compiler *compiler, int op,
	int dst, sljit_w dstw,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w);

/* Label and jump instructions. */

SLJIT_API_FUNC_ATTRIBUTE struct sljit_label* sljit_emit_label(struct sljit_compiler *compiler);

/* Invert conditional instruction: xor (^) with 0x1 */
#define SLJIT_C_EQUAL			0
#define SLJIT_C_ZERO			0
#define SLJIT_C_NOT_EQUAL		1
#define SLJIT_C_NOT_ZERO		1

#define SLJIT_C_LESS			2
#define SLJIT_C_GREATER_EQUAL		3
#define SLJIT_C_GREATER			4
#define SLJIT_C_LESS_EQUAL		5
#define SLJIT_C_SIG_LESS		6
#define SLJIT_C_SIG_GREATER_EQUAL	7
#define SLJIT_C_SIG_GREATER		8
#define SLJIT_C_SIG_LESS_EQUAL		9

#define SLJIT_C_OVERFLOW		10
#define SLJIT_C_NOT_OVERFLOW		11

#define SLJIT_C_MUL_OVERFLOW		12
#define SLJIT_C_MUL_NOT_OVERFLOW	13

#define SLJIT_C_FLOAT_EQUAL		14
#define SLJIT_C_FLOAT_NOT_EQUAL		15
#define SLJIT_C_FLOAT_LESS		16
#define SLJIT_C_FLOAT_GREATER_EQUAL	17
#define SLJIT_C_FLOAT_GREATER		18
#define SLJIT_C_FLOAT_LESS_EQUAL	19
#define SLJIT_C_FLOAT_NAN		20
#define SLJIT_C_FLOAT_NOT_NAN		21

#define SLJIT_JUMP			22
#define SLJIT_FAST_CALL			23
#define SLJIT_CALL0			24
#define SLJIT_CALL1			25
#define SLJIT_CALL2			26
#define SLJIT_CALL3			27

/* Fast calling method. See sljit_emit_fast_enter / sljit_emit_fast_return. */

/* The target can be changed during runtime (see: sljit_set_jump_addr). */
#define SLJIT_REWRITABLE_JUMP		0x1000

/* Emit a jump instruction. The destination is not set, only the type of the jump.
    type must be between SLJIT_C_EQUAL and SLJIT_CALL3
    type can be combined (or'ed) with SLJIT_REWRITABLE_JUMP
   Flags: - (never set any flags) for both conditional and unconditional jumps.
   Flags: destroy all flags for calls. */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_jump(struct sljit_compiler *compiler, int type);

/* Basic arithmetic comparison. In most architectures it is implemented as
   an SLJIT_SUB operation (with SLJIT_UNUSED destination and setting
   appropriate flags) followed by a sljit_emit_jump. However some
   architectures (i.e: MIPS) may employ special optimizations here. It is
   suggested to use this comparison form when appropriate.
    type must be between SLJIT_C_EQUAL and SLJIT_C_SIG_LESS_EQUAL
    type can be combined (or'ed) with SLJIT_REWRITABLE_JUMP or SLJIT_INT_OP
   Flags: destroy flags. */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_cmp(struct sljit_compiler *compiler, int type,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w);

/* Basic floating point comparison. In most architectures it is implemented as
   an SLJIT_FCMP operation (setting appropriate flags) followed by a
   sljit_emit_jump. However some architectures (i.e: MIPS) may employ
   special optimizations here. It is suggested to use this comparison form
   when appropriate.
    type must be between SLJIT_C_FLOAT_EQUAL and SLJIT_C_FLOAT_NOT_NAN
    type can be combined (or'ed) with SLJIT_REWRITABLE_JUMP
   Flags: destroy flags.
   Note: if either operand is NaN, the behaviour is undefined for
         type <= SLJIT_C_FLOAT_LESS_EQUAL. */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_jump* sljit_emit_fcmp(struct sljit_compiler *compiler, int type,
	int src1, sljit_w src1w,
	int src2, sljit_w src2w);

/* Set the destination of the jump to this label. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_set_label(struct sljit_jump *jump, struct sljit_label* label);
/* Only for jumps defined with SLJIT_REWRITABLE_JUMP flag.
   Note: use sljit_emit_ijump for fixed jumps. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_set_target(struct sljit_jump *jump, sljit_uw target);

/* Call function or jump anywhere. Both direct and indirect form
    type must be between SLJIT_JUMP and SLJIT_CALL3
    Direct form: set src to SLJIT_IMM() and srcw to the address
    Indirect form: any other valid addressing mode
   Flags: - (never set any flags) for unconditional jumps.
   Flags: destroy all flags for calls. */
SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_ijump(struct sljit_compiler *compiler, int type, int src, sljit_w srcw);

/* If op == SLJIT_MOV:
     Set dst to 1 if condition is fulfilled, 0 otherwise
       type must be between SLJIT_C_EQUAL and SLJIT_C_FLOAT_NOT_NAN
     Flags: - (never set any flags)
   If op == SLJIT_OR
     Dst is used as src as well, and set its lowest bit to 1 if
     the condition is fulfilled. Otherwise it does nothing.
     Flags: E | K
   Note: sljit_emit_cond_value does nothing, if dst is SLJIT_UNUSED (regardless of op). */
SLJIT_API_FUNC_ATTRIBUTE int sljit_emit_cond_value(struct sljit_compiler *compiler, int op, int dst, sljit_w dstw, int type);

/* The constant can be changed runtime (see: sljit_set_const)
   Flags: - (never set any flags) */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_const* sljit_emit_const(struct sljit_compiler *compiler, int dst, sljit_w dstw, sljit_w init_value);

/* After the code generation the address for label, jump and const instructions
   are computed. Since these structures are freed sljit_free_compiler, the
   addresses must be preserved by the user program elsewere. */
static SLJIT_INLINE sljit_uw sljit_get_label_addr(struct sljit_label *label) { return label->addr; }
static SLJIT_INLINE sljit_uw sljit_get_jump_addr(struct sljit_jump *jump) { return jump->addr; }
static SLJIT_INLINE sljit_uw sljit_get_const_addr(struct sljit_const *const_) { return const_->addr; }

/* Only the address is required to rewrite the code. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_set_jump_addr(sljit_uw addr, sljit_uw new_addr);
SLJIT_API_FUNC_ATTRIBUTE void sljit_set_const(sljit_uw addr, sljit_w new_constant);

/* --------------------------------------------------------------------- */
/*  Miscellaneous utility functions                                      */
/* --------------------------------------------------------------------- */

#define SLJIT_MAJOR_VERSION	0
#define SLJIT_MINOR_VERSION	87

/* Get the human readable name of the platfrom.
   Can be useful for debugging on platforms like ARM, where ARM and
   Thumb2 functions can be mixed. */
SLJIT_API_FUNC_ATTRIBUTE SLJIT_CONST char* sljit_get_platform_name(void);

/* Portble helper function to get an offset of a member. */
#define SLJIT_OFFSETOF(base, member) ((sljit_w)(&((base*)0x10)->member) - 0x10)

#if (defined SLJIT_UTIL_GLOBAL_LOCK && SLJIT_UTIL_GLOBAL_LOCK)
/* This global lock is useful to compile common functions. */
SLJIT_API_FUNC_ATTRIBUTE void SLJIT_CALL sljit_grab_lock(void);
SLJIT_API_FUNC_ATTRIBUTE void SLJIT_CALL sljit_release_lock(void);
#endif

#if (defined SLJIT_UTIL_STACK && SLJIT_UTIL_STACK)

/* The sljit_stack is a utiliy feature of sljit, which allocates a
   writable memory region between base (inclusive) and limit (exclusive).
   Both base and limit is a pointer, and base is always <= than limit.
   This feature uses the "address space reserve" feature
   of modern operating systems. Basically we don't need to allocate a
   huge memory block in one step for the worst case, we can start with
   a smaller chunk and extend it later. Since the address space is
   reserved, the data never copied to other regions, thus it is safe
   to store pointers here. */

/* Note: The base field is aligned to PAGE_SIZE bytes (usually 4k or more).
   Note: stack growing should not happen in small steps: 4k, 16k or even
     bigger growth is better.
   Note: this structure may not be supported by all operating systems.
     Some kind of fallback mechanism is suggested when SLJIT_UTIL_STACK
     is not defined. */

struct sljit_stack {
	/* User data, anything can be stored here.
	   Starting with the same value as base. */
	sljit_uw top;
	/* These members are read only. */
	sljit_uw base;
	sljit_uw limit;
	sljit_uw max_limit;
};

/* Returns NULL if unsuccessful.
   Note: limit and max_limit contains the size for stack allocation
   Note: the top field is initialized to base. */
SLJIT_API_FUNC_ATTRIBUTE struct sljit_stack* SLJIT_CALL sljit_allocate_stack(sljit_uw limit, sljit_uw max_limit);
SLJIT_API_FUNC_ATTRIBUTE void SLJIT_CALL sljit_free_stack(struct sljit_stack* stack);

/* Can be used to increase (allocate) or decrease (free) the memory area.
   Returns with a non-zero value if unsuccessful. If new_limit is greater than
   max_limit, it will fail. It is very easy to implement a stack data structure,
   since the growth ratio can be added to the current limit, and sljit_stack_resize
   will do all the necessary checks. The fields of the stack are not changed if
   sljit_stack_resize fails. */
SLJIT_API_FUNC_ATTRIBUTE sljit_w SLJIT_CALL sljit_stack_resize(struct sljit_stack* stack, sljit_uw new_limit);

#endif /* (defined SLJIT_UTIL_STACK && SLJIT_UTIL_STACK) */

#if !(defined SLJIT_INDIRECT_CALL && SLJIT_INDIRECT_CALL)

/* Get the entry address of a given function. */
#define SLJIT_FUNC_OFFSET(func_name)	((sljit_w)func_name)

#else /* !(defined SLJIT_INDIRECT_CALL && SLJIT_INDIRECT_CALL) */

/* All JIT related code should be placed in the same context (library, binary, etc.). */

#define SLJIT_FUNC_OFFSET(func_name)	((sljit_w)*(void**)func_name)

/* For powerpc64, the function pointers point to a context descriptor. */
struct sljit_function_context {
	sljit_w addr;
	sljit_w r2;
	sljit_w r11;
};

/* Fill the context arguments using the addr and the function.
   If func_ptr is NULL, it will not be set to the address of context
   If addr is NULL, the function address also comes from the func pointer. */
SLJIT_API_FUNC_ATTRIBUTE void sljit_set_function_context(void** func_ptr, struct sljit_function_context* context, sljit_w addr, void* func);

#endif /* !(defined SLJIT_INDIRECT_CALL && SLJIT_INDIRECT_CALL) */

#endif /* _SLJIT_LIR_H_ */
