/* include/config.h.  Generated from config.h.in by configure.  */
/* include/config.h.in.  Generated from configure.ac by autoheader.  */

/* Block signals before mutex operations */
#define CONFIG_BLOCK_SIGNALS /**/

/* Enable Debug Frame */
#define CONFIG_DEBUG_FRAME /**/

/* Support for Microsoft ABI extensions */
/* #undef CONFIG_MSABI_SUPPORT */

/* Define if the weak 'backtrace' symbol is provided. */
#define CONFIG_WEAK_BACKTRACE 1

/* Define to 1 if you want every memory access validated */
#define CONSERVATIVE_CHECKS 1

/* Define to 1 if you have the <asm/ptrace.h> header file. */
#define HAVE_ASM_PTRACE_H 1

/* Define to 1 if you have the <asm/ptrace_offsets.h> header file. */
/* #undef HAVE_ASM_PTRACE_OFFSETS_H */

/* Define to 1 if you have the <asm/vsyscall.h> header file. */
/* #undef HAVE_ASM_VSYSCALL_H */

/* Define to 1 if you have the <byteswap.h> header file. */
#define HAVE_BYTESWAP_H 1

/* Define to 1 if you have the declaration of `PTRACE_CONT', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PTRACE_CONT */

/* Define to 1 if you have the declaration of `PTRACE_POKEDATA', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_POKEDATA */

/* Define to 1 if you have the declaration of `PTRACE_POKEUSER', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_POKEUSER */

/* Define to 1 if you have the declaration of `PTRACE_SETREGSET', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_SETREGSET */

/* Define to 1 if you have the declaration of `PTRACE_SINGLESTEP', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_SINGLESTEP */

/* Define to 1 if you have the declaration of `PTRACE_SYSCALL', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_SYSCALL */

/* Define to 1 if you have the declaration of `PTRACE_TRACEME', and to 0 if
   you don't. */
/* #undef HAVE_DECL_PTRACE_TRACEME */

/* Define to 1 if you have the declaration of `PT_CONTINUE', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_CONTINUE */

/* Define to 1 if you have the declaration of `PT_GETFPREGS', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_GETFPREGS */

/* Define to 1 if you have the declaration of `PT_GETREGS', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_GETREGS */

/* Define to 1 if you have the declaration of `PT_IO', and to 0 if you don't.
 */
/* #undef HAVE_DECL_PT_IO */

/* Define to 1 if you have the declaration of `PT_STEP', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_STEP */

/* Define to 1 if you have the declaration of `PT_SYSCALL', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_SYSCALL */

/* Define to 1 if you have the declaration of `PT_TRACE_ME', and to 0 if you
   don't. */
/* #undef HAVE_DECL_PT_TRACE_ME */

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the `dlmodinfo' function. */
/* #undef HAVE_DLMODINFO */

/* Define to 1 if you have the `dl_iterate_phdr' function. */
#define HAVE_DL_ITERATE_PHDR 1

/* Define to 1 if you have the `dl_phdr_removals_counter' function. */
/* #undef HAVE_DL_PHDR_REMOVALS_COUNTER */

/* Define to 1 if the system has the type `elf_fpregset_t'. */
#define HAVE_ELF_FPREGSET_T 1

/* Define to 1 if you have the <elf.h> header file. */
#define HAVE_ELF_H 1

/* Define to 1 if you have the <endian.h> header file. */
#define HAVE_ENDIAN_H 1

/* Define to 1 if you have the <execinfo.h> header file. */
#define HAVE_EXECINFO_H 1

/* Define to 1 if you have the `execvpe' function. */
#define HAVE_EXECVPE 1

/* Define to 1 if you have the `getunwind' function. */
/* #undef HAVE_GETUNWIND */

/* Define to 1 if you have the <ia64intrin.h> header file. */
/* #undef HAVE_IA64INTRIN_H */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <link.h> header file. */
#define HAVE_LINK_H 1

/* Define if you have liblzma */
/* #undef HAVE_LZMA */

/* Define to 1 if you have the `mincore' function. */
#define HAVE_MINCORE 1

/* Define to 1 if you have the `pipe2' function. */
#define HAVE_PIPE2 1

/* Define to 1 if the system has the type `procfs_status'. */
/* #undef HAVE_PROCFS_STATUS */

/* Define to 1 if you have the `sigaltstack' function. */
#define HAVE_SIGALTSTACK 1

/* Define to 1 if you have the <signal.h> header file. */
#define HAVE_SIGNAL_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if `dlpi_subs' is a member of `struct dl_phdr_info'. */
#define HAVE_STRUCT_DL_PHDR_INFO_DLPI_SUBS 1

/* Define to 1 if the system has the type `struct elf_prstatus'. */
#define HAVE_STRUCT_ELF_PRSTATUS 1

/* Define to 1 if the system has the type `struct prstatus'. */
/* #undef HAVE_STRUCT_PRSTATUS */

/* Define to 1 if you have the <sys/elf.h> header file. */
#define HAVE_SYS_ELF_H 1

/* Define to 1 if you have the <sys/endian.h> header file. */
/* #undef HAVE_SYS_ENDIAN_H */

/* Define to 1 if you have the <sys/link.h> header file. */
/* #undef HAVE_SYS_LINK_H */

/* Define to 1 if you have the <sys/param.h> header file. */
#define HAVE_SYS_PARAM_H 1

/* Define to 1 if you have the <sys/procfs.h> header file. */
#define HAVE_SYS_PROCFS_H 1

/* Define to 1 if you have the <sys/ptrace.h> header file. */
#define HAVE_SYS_PTRACE_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/syscall.h> header file. */
#define HAVE_SYS_SYSCALL_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/uc_access.h> header file. */
/* #undef HAVE_SYS_UC_ACCESS_H */

/* Define to 1 if you have the `ttrace' function. */
/* #undef HAVE_TTRACE */

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define if you have libz */
/* #undef HAVE_ZLIB */

/* Defined if __builtin_unreachable() is available */
#define HAVE__BUILTIN_UNREACHABLE 1

/* Defined if __builtin___clear_cache() is available */
#define HAVE__BUILTIN___CLEAR_CACHE 1

/* Define to 1 if --enable-per-thread-cache */
/* #undef HAVE___CACHE_PER_THREAD */

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "libunwind"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "https://github.com/libunwind/libunwind"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libunwind"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libunwind 1.8.1"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libunwind"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.8.1"

/* The size of `off_t', as computed by sizeof. */
#define SIZEOF_OFF_T 8

/* Define to 1 if all of the C90 standard headers exist (not just the ones
   required in a freestanding environment). This macro is provided for
   backward compatibility; new code need not use it. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "1.8.1"

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to `__inline__' or `__inline' if that's what the C compiler
   calls it, or to nothing if 'inline' is not supported under any name.  */
#ifndef __cplusplus
/* #undef inline */
#endif

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */
