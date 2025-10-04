/* libunwind - a platform-independent unwind library
   Copyright (C) 2001-2005 Hewlett-Packard Co
   Copyright (C) 2007 David Mosberger-Tang
        Contributed by David Mosberger-Tang <dmosberger@gmail.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

/* This files contains libunwind-internal definitions which are
   subject to frequent change and are not to be exposed to
   libunwind-users.  */

#ifndef libunwind_i_h
#define libunwind_i_h

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "compiler.h"

#if defined(HAVE___CACHE_PER_THREAD) && HAVE___CACHE_PER_THREAD
#define UNWI_DEFAULT_CACHING_POLICY UNW_CACHE_PER_THREAD
#else
#define UNWI_DEFAULT_CACHING_POLICY UNW_CACHE_GLOBAL
#endif

/* Platform-independent libunwind-internal declarations.  */

#include <sys/types.h>  /* HP-UX needs this before include of pthread.h */

#include <assert.h>
#include <libunwind.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>

#if defined(HAVE_SYS_SYSCALL_H)
# include <sys/syscall.h>   /* For SYS_xxx definitions */
#endif

#if defined(HAVE_ELF_H)
# include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
# include <sys/elf.h>
#else
# error Could not locate <elf.h>
#endif
#if defined(ELFCLASS32)
# define UNW_ELFCLASS32 ELFCLASS32
#else
# define UNW_ELFCLASS32 1
#endif
#if defined(ELFCLASS64)
# define UNW_ELFCLASS64 ELFCLASS64
#else
# define UNW_ELFCLASS64 2
#endif

#if defined(HAVE_ENDIAN_H)
# include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
# include <sys/endian.h>
#elif defined(HAVE_SYS_PARAM_H)
# include <sys/param.h>
#endif

#if defined(__LITTLE_ENDIAN)
# define UNW_LITTLE_ENDIAN __LITTLE_ENDIAN
#elif defined(_LITTLE_ENDIAN)
# define UNW_LITTLE_ENDIAN _LITTLE_ENDIAN
#elif defined(LITTLE_ENDIAN)
# define UNW_LITTLE_ENDIAN LITTLE_ENDIAN
#else
# define UNW_LITTLE_ENDIAN 1234
#endif

#if defined(__BIG_ENDIAN)
# define UNW_BIG_ENDIAN __BIG_ENDIAN
#elif defined(_BIG_ENDIAN)
# define UNW_BIG_ENDIAN _BIG_ENDIAN
#elif defined(BIG_ENDIAN)
# define UNW_BIG_ENDIAN BIG_ENDIAN
#else
# define UNW_BIG_ENDIAN 4321
#endif

#if defined(__BYTE_ORDER)
# define UNW_BYTE_ORDER __BYTE_ORDER
#elif defined(_BYTE_ORDER)
# define UNW_BYTE_ORDER _BYTE_ORDER
#elif defined(BIG_ENDIAN)
# define UNW_BYTE_ORDER BYTE_ORDER
#else
# if defined(__hpux)
#  define UNW_BYTE_ORDER UNW_BIG_ENDIAN
# else
#  error Target has unknown byte ordering.
# endif
#endif

static inline int
byte_order_is_valid(int byte_order)
{
    return byte_order == UNW_BIG_ENDIAN
        || byte_order == UNW_LITTLE_ENDIAN;
}

static inline int
byte_order_is_big_endian(int byte_order)
{
    return byte_order == UNW_BIG_ENDIAN;
}

static inline int
target_is_big_endian(void)
{
    return byte_order_is_big_endian(UNW_BYTE_ORDER);
}

#if defined(HAVE__BUILTIN_UNREACHABLE)
# define unreachable() __builtin_unreachable()
#else
# define unreachable() do { } while (1)
#endif

#ifdef DEBUG
# define UNW_DEBUG      1
#else
# undef UNW_DEBUG
#endif

/* Make it easy to write thread-safe code which may or may not be
   linked against libpthread.  The macros below can be used
   unconditionally and if -lpthread is around, they'll call the
   corresponding routines otherwise, they do nothing.  */

#pragma weak pthread_mutex_init
#pragma weak pthread_mutex_lock
#pragma weak pthread_mutex_unlock
#pragma weak pthread_sigmask

#define mutex_init(l)                                                   \
        (pthread_mutex_init != NULL ? pthread_mutex_init ((l), NULL) : 0)
#define mutex_lock(l)                                                   \
        (pthread_mutex_lock != NULL ? pthread_mutex_lock (l) : 0)
#define mutex_unlock(l)                                                 \
        (pthread_mutex_unlock != NULL ? pthread_mutex_unlock (l) : 0)

#define UNWI_OBJ(fn)      UNW_PASTE(UNW_PREFIX,UNW_PASTE(I,fn))
#define UNWI_ARCH_OBJ(fn) UNW_PASTE(UNW_PASTE(UNW_PASTE(_UI,UNW_TARGET),_), fn)

#define unwi_full_mask    UNWI_ARCH_OBJ(full_mask)

/* Type of a mask that can be used to inhibit preemption.  At the
   userlevel, preemption is caused by signals and hence sigset_t is
   appropriate.  In contrast, the Linux kernel uses "unsigned long"
   to hold the processor "flags" instead.  */
typedef sigset_t intrmask_t;

extern intrmask_t unwi_full_mask;

/* Silence compiler warnings about variables which are used only if libunwind
   is configured in a certain way */
static inline void mark_as_used(void *v UNUSED) {
}

#if defined(CONFIG_BLOCK_SIGNALS)
/* SIGPROCMASK ignores return values, so we do not have to correct for pthread_sigmask() returning
   errno on failure when sigprocmask() returns -1. */
# define SIGPROCMASK(how, new_mask, old_mask) \
    (pthread_sigmask != NULL ? pthread_sigmask((how), (new_mask), (old_mask)) \
     : sigprocmask((how), (new_mask), (old_mask)))
#else
# define SIGPROCMASK(how, new_mask, old_mask) mark_as_used(old_mask)
#endif

/* Prefer adaptive mutexes if available */
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#define UNW_PTHREAD_MUTEX_INITIALIZER PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
#else
#define UNW_PTHREAD_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif

#define define_lock(name) \
  pthread_mutex_t name = UNW_PTHREAD_MUTEX_INITIALIZER
#define lock_init(l)            mutex_init (l)
#define lock_acquire(l,m)                               \
do {                                                    \
  SIGPROCMASK (SIG_SETMASK, &unwi_full_mask, &(m));     \
  mutex_lock (l);                                       \
} while (0)
#define lock_release(l,m)                       \
do {                                            \
  mutex_unlock (l);                             \
  SIGPROCMASK (SIG_SETMASK, &(m), NULL);        \
} while (0)

#define SOS_MEMORY_SIZE 16384   /* see src/mi/mempool.c */

/* Provide an internal syscall version of mmap to improve signal safety. */
static ALWAYS_INLINE void *
mi_mmap (void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
#if defined(SYS_mmap) && !defined(__i386__)
  /* Where supported, bypass libc and invoke the syscall directly. */
# if defined(__FreeBSD__) // prefer over syscall on *BSD
  long int ret = __syscall (SYS_mmap, addr, len, prot, flags, fd, offset);
# else
  long int ret = syscall (SYS_mmap, addr, len, prot, flags, fd, offset);
# endif
  // @todo this is very likely Linux specific
  if ((unsigned long int)ret > -4096UL)
    return MAP_FAILED;
  else
    return (void *)ret;
#else
  /* Where direct syscalls are not supported, forward to the libc call. */
  return mmap (addr, len, prot, flags, fd, offset);
#endif
}

/* Provide an internal syscall version of munmap to improve signal safety. */
static ALWAYS_INLINE int
mi_munmap (void *addr, size_t len)
{
#ifdef SYS_munmap
  return syscall (SYS_munmap, addr, len);
#else
  return munmap (addr, len);
#endif
}

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif
#define GET_MEMORY(mem, size)                                               \
do {                                                                        \
  mem = mi_mmap (NULL, size, PROT_READ | PROT_WRITE,                        \
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);                       \
  if (mem == MAP_FAILED)                                                    \
    mem = NULL;                                                             \
} while (0)

#define unwi_find_dynamic_proc_info     UNWI_OBJ(find_dynamic_proc_info)
#define unwi_extract_dynamic_proc_info  UNWI_OBJ(extract_dynamic_proc_info)
#define unwi_put_dynamic_unwind_info    UNWI_OBJ(put_dynamic_unwind_info)
#define unwi_dyn_remote_find_proc_info  UNWI_OBJ(dyn_remote_find_proc_info)
#define unwi_dyn_remote_put_unwind_info UNWI_OBJ(dyn_remote_put_unwind_info)
#define unwi_dyn_validate_cache         UNWI_OBJ(dyn_validate_cache)

extern int unwi_find_dynamic_proc_info (unw_addr_space_t as,
                                        unw_word_t ip,
                                        unw_proc_info_t *pi,
                                        int need_unwind_info, void *arg);
extern int unwi_extract_dynamic_proc_info (unw_addr_space_t as,
                                           unw_word_t ip,
                                           unw_proc_info_t *pi,
                                           unw_dyn_info_t *di,
                                           int need_unwind_info,
                                           void *arg);
extern void unwi_put_dynamic_unwind_info (unw_addr_space_t as,
                                          unw_proc_info_t *pi, void *arg);

/* These handle the remote (cross-address-space) case of accessing
   dynamic unwind info. */

extern int unwi_dyn_remote_find_proc_info (unw_addr_space_t as,
                                           unw_word_t ip,
                                           unw_proc_info_t *pi,
                                           int need_unwind_info,
                                           void *arg);
extern void unwi_dyn_remote_put_unwind_info (unw_addr_space_t as,
                                             unw_proc_info_t *pi,
                                             void *arg);
extern int unwi_dyn_validate_cache (unw_addr_space_t as, void *arg);

extern unw_dyn_info_list_t _U_dyn_info_list;
extern pthread_mutex_t _U_dyn_info_list_lock;

#define unw_address_is_valid UNWI_ARCH_OBJ(address_is_valid)
HIDDEN bool unw_address_is_valid(unw_word_t, size_t);


#if defined(UNW_DEBUG)
# define unwi_debug_level                UNWI_ARCH_OBJ(debug_level)
extern long unwi_debug_level;

# include <stdarg.h>
# include <stdio.h>
# include <unistd.h>

#define Debug(level, ...) _unw_debug(level, __FUNCTION__,  __VA_ARGS__)

/**
 * Send a debug message to stderr.
 *
 * This function may be called from within a signal handler context where
 * fprintf(3) is not safe to call. The write(2) call is safe, however, and we're
 * going to have to assume that snprintf(3) is signal safe otherwise it's pretty
 * pointless to use Debug() calls anywhere.
 */
static inline void _unw_debug(int level, char const * const fname, char const * const fmt, ...)
{
  if (unwi_debug_level >= level)
    {
      enum { buf_size = 512 };
      char buf[buf_size];

      if (level > 16) level = 16;
      int bcount = snprintf (buf, buf_size, "%*c>%s: ", level, ' ', fname);
      int res = write(STDERR_FILENO, buf, bcount);

      va_list ap;
      va_start(ap, fmt);
      bcount = vsnprintf (buf, buf_size, fmt, ap);
      va_end(ap);
      res = write(STDERR_FILENO, buf, bcount);
      (void)res; /* silence "variable set but not used" warning */
    }
}
# define Dprintf(/* format */ ...)                                      \
  fprintf (stderr, /* format */ __VA_ARGS__)
#else /* defined(UNW_DEBUG) */
# define Debug(level, /* format */ ...)
# define Dprintf( /* format */ ...)
#endif /* defined(UNW_DEBUG) */

static ALWAYS_INLINE int
print_error (const char *string)
{
  return write (2, string, strlen (string));
}

HIDDEN extern long unw_page_size;

static inline unw_word_t unw_page_start(unw_word_t addr)
{
  return addr & ~(unw_page_size - 1);
}

#define mi_init         UNWI_ARCH_OBJ(mi_init)

extern void mi_init (void);     /* machine-independent initializations */
extern unw_word_t _U_dyn_info_list_addr (void);

/* This is needed/used by ELF targets only.  */

struct elf_image
  {
    void *image;                /* pointer to mmap'd image */
    size_t size;                /* (file-) size of the image */
  };

struct elf_dyn_info
  {
    struct elf_image ei;
    unw_dyn_info_t di_cache;
    unw_dyn_info_t di_debug;    /* additional table info for .debug_frame */
#if UNW_TARGET_IA64
    unw_dyn_info_t ktab;
#endif
#if UNW_TARGET_ARM
    unw_dyn_info_t di_arm;      /* additional table info for .ARM.exidx */
#endif
  };

static inline void invalidate_edi (struct elf_dyn_info *edi)
{
  if (edi->ei.image)
    mi_munmap (edi->ei.image, edi->ei.size);
  memset (edi, 0, sizeof (*edi));
  edi->di_cache.format = -1;
  edi->di_debug.format = -1;
#if UNW_TARGET_ARM
  edi->di_arm.format = -1;
#endif
}


/* Provide a place holder for architecture to override for fast access
   to memory when known not to need to validate and know the access
   will be local to the process. A suitable override will improve
   unw_tdep_trace() performance in particular. */
#define ACCESS_MEM_FAST(ret,validate,cur,addr,to) \
  do { (ret) = dwarf_get ((cur), DWARF_MEM_LOC ((cur), (addr)), &(to)); } \
  while (0)

/* Define GNU and processor specific values for the Phdr p_type field in case
   they aren't defined by <elf.h>.  */
#ifndef PT_GNU_EH_FRAME
# define PT_GNU_EH_FRAME        0x6474e550
#endif /* !PT_GNU_EH_FRAME */
#ifndef PT_ARM_EXIDX
# define PT_ARM_EXIDX           0x70000001      /* ARM unwind segment */
#endif /* !PT_ARM_EXIDX */

#include "tdep/libunwind_i.h"

#ifndef TDEP_DWARF_SP
#define TDEP_DWARF_SP UNW_TDEP_SP
#endif

#ifndef tdep_get_func_addr
# define tdep_get_func_addr(as,addr,v)          (*(v) = addr, 0)
#endif

#ifndef DWARF_VAL_LOC
# define DWARF_IS_VAL_LOC(l)    0
# define DWARF_VAL_LOC(c,v)     DWARF_NULL_LOC
#endif

#define UNW_ALIGN(x,a) (((x)+(a)-1UL)&~((a)-1UL))

#endif /* libunwind_i_h */
