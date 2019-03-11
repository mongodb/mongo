#include <ucontext.h>
#include <stddef.h>

extern "C" void getcontext_light(ucontext_t *ctx);

// clang's built-in asm cannot handle .set directives
#if defined(__GNUC__) && !defined(__llvm__) && defined(__x86_64) && defined(_LP64)

#define R(r) offsetof(ucontext_t, uc_mcontext.gregs[r])

static __attribute__((used))
void getcontext_tramp(ucontext_t *ctx) {
	__asm__ __volatile__(".set oRBX, %c0\n"
			     ".set oRBP, %c1\n"
			     ".set oR12, %c2\n"
			     ".set oR13, %c3\n"
			     ".set oR14, %c4\n"
			     ".set oR15, %c5\n"
			     ".set oRIP, %c6\n"
			     ".set oRSP, %c7\n"
			     : :
			       "p" (R(REG_RBX)),
			       "p" (R(REG_RBP)),
			       "p" (R(REG_R12)),
			       "p" (R(REG_R13)),
			       "p" (R(REG_R14)),
			       "p" (R(REG_R15)),
			       "p" (R(REG_RIP)),
			       "p" (R(REG_RSP)));
	getcontext_light(ctx);
}

__asm__(".pushsection .text; .globl getcontext_light\n"
	".type getcontext_light, @function\n"
"getcontext_light:\n"
	"\t.cfi_startproc\n"
	"\tmovq     %rbx, oRBX(%rdi)\n"
	"\tmovq     %rbp, oRBP(%rdi)\n"
	"\tmovq     %r12, oR12(%rdi)\n"
	"\tmovq     %r13, oR13(%rdi)\n"
	"\tmovq     %r14, oR14(%rdi)\n"
	"\tmovq     %r15, oR14(%rdi)\n"

	"\tmovq     (%rsp), %rcx\n"
	"\tmovq     %rcx, oRIP(%rdi)\n"
	"\tleaq     8(%rsp), %rcx\n"                /* Exclude the return address.  */
	"\tmovq     %rcx, oRSP(%rdi)\n"
	"\tret\n"
	".cfi_endproc\n"
	".size getcontext_light, .-getcontext_light\n"
	".popsection\n"
	);

#else

extern "C" void getcontext_light(ucontext_t *ctx) {
	getcontext(ctx);
}

#endif
