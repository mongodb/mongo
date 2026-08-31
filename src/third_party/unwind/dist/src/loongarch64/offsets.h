/* Linux-specific definitions: */

/* Define various structure offsets to simplify cross-compilation.  */

/* Offsets for LoongArch64 Linux "ucontext_t":  */

# define LINUX_UC_FLAGS_OFF		0x0	/* offsetof(struct ucontext_t, __uc_flags) */
# define LINUX_UC_LINK_OFF		0x8	/* offsetof(struct ucontext_t, uc_link) */
# define LINUX_UC_STACK_OFF		0x10	/* offsetof(struct ucontext_t, uc_stack) */
# define LINUX_UC_SIGMASK_OFF		0x28	/* offsetof(struct ucontext_t, uc_sigmask) */
# define LINUX_UC_MCONTEXT_OFF		0xb0	/* offsetof(struct ucontext_t, uc_mcontext) */

# define LINUX_UC_MCONTEXT_PC		0xb0	/* offsetof(struct ucontext_t, uc_mcontext.__pc) */
# define LINUX_UC_MCONTEXT_GREGS	0xb8	/* offsetof(struct ucontext_t, uc_mcontext.__gregs) */
/* Offsets for LoongArch64 Linux "struct sigcontext": */
#define LINUX_SC_R0_OFF   (LINUX_UC_MCONTEXT_GREGS - LINUX_UC_MCONTEXT_OFF)
#define LINUX_SC_R1_OFF   (LINUX_SC_R0_OFF + 1*8)
#define LINUX_SC_R2_OFF   (LINUX_SC_R0_OFF + 2*8)
#define LINUX_SC_R3_OFF   (LINUX_SC_R0_OFF + 3*8)
#define LINUX_SC_R4_OFF   (LINUX_SC_R0_OFF + 4*8)
#define LINUX_SC_R5_OFF   (LINUX_SC_R0_OFF + 5*8)
#define LINUX_SC_R6_OFF   (LINUX_SC_R0_OFF + 6*8)
#define LINUX_SC_R7_OFF   (LINUX_SC_R0_OFF + 7*8)
#define LINUX_SC_R8_OFF   (LINUX_SC_R0_OFF + 8*8)
#define LINUX_SC_R9_OFF   (LINUX_SC_R0_OFF + 9*8)
#define LINUX_SC_R10_OFF  (LINUX_SC_R0_OFF + 10*8)
#define LINUX_SC_R11_OFF  (LINUX_SC_R0_OFF + 11*8)
#define LINUX_SC_R12_OFF  (LINUX_SC_R0_OFF + 12*8)
#define LINUX_SC_R13_OFF  (LINUX_SC_R0_OFF + 13*8)
#define LINUX_SC_R14_OFF  (LINUX_SC_R0_OFF + 14*8)
#define LINUX_SC_R15_OFF  (LINUX_SC_R0_OFF + 15*8)
#define LINUX_SC_R16_OFF  (LINUX_SC_R0_OFF + 16*8)
#define LINUX_SC_R17_OFF  (LINUX_SC_R0_OFF + 17*8)
#define LINUX_SC_R18_OFF  (LINUX_SC_R0_OFF + 18*8)
#define LINUX_SC_R19_OFF  (LINUX_SC_R0_OFF + 19*8)
#define LINUX_SC_R20_OFF  (LINUX_SC_R0_OFF + 20*8)
#define LINUX_SC_R21_OFF  (LINUX_SC_R0_OFF + 21*8)
#define LINUX_SC_R22_OFF  (LINUX_SC_R0_OFF + 22*8)
#define LINUX_SC_R23_OFF  (LINUX_SC_R0_OFF + 23*8)
#define LINUX_SC_R24_OFF  (LINUX_SC_R0_OFF + 24*8)
#define LINUX_SC_R25_OFF  (LINUX_SC_R0_OFF + 25*8)
#define LINUX_SC_R26_OFF  (LINUX_SC_R0_OFF + 26*8)
#define LINUX_SC_R27_OFF  (LINUX_SC_R0_OFF + 27*8)
#define LINUX_SC_R28_OFF  (LINUX_SC_R0_OFF + 28*8)
#define LINUX_SC_R29_OFF  (LINUX_SC_R0_OFF + 29*8)
#define LINUX_SC_R30_OFF  (LINUX_SC_R0_OFF + 30*8)
#define LINUX_SC_R31_OFF  (LINUX_SC_R0_OFF + 31*8)

#define LINUX_SC_SP_OFF   LINUX_SC_R3_OFF
#define LINUX_SC_PC_OFF   (LINUX_UC_MCONTEXT_PC - LINUX_UC_MCONTEXT_OFF)
