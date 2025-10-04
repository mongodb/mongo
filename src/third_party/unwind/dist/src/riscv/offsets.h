#ifdef __linux__

/* Linux-specific definitions: */

/* The RISC-V ucontext has the following structure:

   https://github.com/torvalds/linux/blob/44db63d1ad8d71c6932cbe007eb41f31c434d140/arch/riscv/include/uapi/asm/ucontext.h
*/
#define UC_MCONTEXT_REGS_OFF 176

#else
# error "Unsupported OS"
#endif
