/* libunwind - a platform-independent unwind library
   Copyright (C) 2002-2005 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>

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

#ifndef S390X_LIBUNWIND_I_H
#define S390X_LIBUNWIND_I_H

/* Target-dependent definitions that are internal to libunwind but need
   to be shared with target-independent code.  */

#include <stdlib.h>
#include <libunwind.h>
#include <stdatomic.h>

#include "elf64.h"
#include "mempool.h"
#include "dwarf.h"

struct unw_addr_space
  {
    struct unw_accessors acc;
    unw_caching_policy_t caching_policy;
    _Atomic uint32_t cache_generation;
    unw_word_t dyn_generation;          /* see dyn-common.h */
    unw_word_t dyn_info_list_addr;      /* (cached) dyn_info_list_addr */
    struct dwarf_rs_cache global_cache;
    struct unw_debug_frame_list *debug_frames;
   };

struct cursor
  {
    struct dwarf_cursor dwarf;          /* must be first */

    /* Format of sigcontext structure and address at which it is
       stored: */
    enum
      {
        S390X_SCF_NONE              = 0, /* no signal frame encountered */
        S390X_SCF_LINUX_SIGFRAME    = 1, /* Linux struct sigcontext */
        S390X_SCF_LINUX_RT_SIGFRAME = 2, /* Linux ucontext_t */
      }
    sigcontext_format;
    unw_word_t sigcontext_addr;
    unw_word_t sigcontext_sp;
    unw_word_t sigcontext_pc;
    int validate;
    ucontext_t *uc;
  };

static inline ucontext_t *
dwarf_get_uc(const struct dwarf_cursor *cursor)
{
  const struct cursor *c = (struct cursor *) cursor->as_arg;
  return c->uc;
}

#define DWARF_GET_LOC(l)        ((l).val)
# define DWARF_LOC_TYPE_MEM     (0 << 0)
# define DWARF_LOC_TYPE_FP      (1 << 0)
# define DWARF_LOC_TYPE_REG     (1 << 1)
# define DWARF_LOC_TYPE_VAL     (1 << 2)

# define DWARF_IS_REG_LOC(l)    (((l).type & DWARF_LOC_TYPE_REG) != 0)
# define DWARF_IS_FP_LOC(l)     (((l).type & DWARF_LOC_TYPE_FP) != 0)
# define DWARF_IS_MEM_LOC(l)    ((l).type == DWARF_LOC_TYPE_MEM)
# define DWARF_IS_VAL_LOC(l)    (((l).type & DWARF_LOC_TYPE_VAL) != 0)

# define DWARF_LOC(r, t)        ((dwarf_loc_t) { .val = (r), .type = (t) })
# define DWARF_VAL_LOC(c,v)     DWARF_LOC ((v), DWARF_LOC_TYPE_VAL)
# define DWARF_MEM_LOC(c,m)     DWARF_LOC ((m), DWARF_LOC_TYPE_MEM)

#ifdef UNW_LOCAL_ONLY
# define DWARF_NULL_LOC         DWARF_LOC (0, 0)
# define DWARF_IS_NULL_LOC(l)   (DWARF_GET_LOC (l) == 0)
# define DWARF_REG_LOC(c,r)     (DWARF_LOC((unw_word_t)                      \
                                 tdep_uc_addr(dwarf_get_uc(c), (r)), 0))
# define DWARF_FPREG_LOC(c,r)   (DWARF_LOC((unw_word_t)                      \
                                 tdep_uc_addr(dwarf_get_uc(c), (r)), 0))

#else /* !UNW_LOCAL_ONLY */

# define DWARF_NULL_LOC         DWARF_LOC (0, 0)
# define DWARF_IS_NULL_LOC(l)                                           \
                ({ dwarf_loc_t _l = (l); _l.val == 0 && _l.type == 0; })
# define DWARF_REG_LOC(c,r)     DWARF_LOC((r), DWARF_LOC_TYPE_REG)
# define DWARF_FPREG_LOC(c,r)   DWARF_LOC((r), (DWARF_LOC_TYPE_REG      \
                                                | DWARF_LOC_TYPE_FP))

#endif /* !UNW_LOCAL_ONLY */

static inline int
dwarf_getfp (struct dwarf_cursor *c, dwarf_loc_t loc, unw_fpreg_t *val)
{
  assert(sizeof(unw_fpreg_t) == sizeof(unw_word_t));

  if (DWARF_IS_NULL_LOC (loc))
    return -UNW_EBADREG;

  if (DWARF_IS_FP_LOC (loc))
    return (*c->as->acc.access_fpreg) (c->as, DWARF_GET_LOC (loc), val,
                                       0, c->as_arg);
  /* FPRs may be saved in GPRs */
  if (DWARF_IS_REG_LOC (loc))
    return (*c->as->acc.access_reg) (c->as, DWARF_GET_LOC (loc), (unw_word_t*)val,
                                     0, c->as_arg);
  if (DWARF_IS_MEM_LOC (loc))
    return (*c->as->acc.access_mem) (c->as, DWARF_GET_LOC (loc), (unw_word_t*)val,
                                     0, c->as_arg);
  assert(DWARF_IS_VAL_LOC (loc));
  *val = *(unw_fpreg_t*) DWARF_GET_LOC (loc);
  return 0;
}

static inline int
dwarf_putfp (struct dwarf_cursor *c, dwarf_loc_t loc, unw_fpreg_t val)
{
  assert(sizeof(unw_fpreg_t) == sizeof(unw_word_t));
  assert(!DWARF_IS_VAL_LOC (loc));

  if (DWARF_IS_NULL_LOC (loc))
    return -UNW_EBADREG;

  if (DWARF_IS_FP_LOC (loc))
    return (*c->as->acc.access_fpreg) (c->as, DWARF_GET_LOC (loc), &val,
                                       1, c->as_arg);
  /* FPRs may be saved in GPRs */
  if (DWARF_IS_REG_LOC (loc))
    return (*c->as->acc.access_reg) (c->as, DWARF_GET_LOC (loc), (unw_word_t*) &val,
                                     1, c->as_arg);

  assert(DWARF_IS_MEM_LOC (loc));
  return (*c->as->acc.access_mem) (c->as, DWARF_GET_LOC (loc), (unw_word_t*) &val,
                                   1, c->as_arg);
}

static inline int
dwarf_get (struct dwarf_cursor *c, dwarf_loc_t loc, unw_word_t *val)
{
  assert(sizeof(unw_fpreg_t) == sizeof(unw_word_t));

  if (DWARF_IS_NULL_LOC (loc))
    return -UNW_EBADREG;

  /* GPRs may be saved in FPRs */
  if (DWARF_IS_FP_LOC (loc))
    return (*c->as->acc.access_fpreg) (c->as, DWARF_GET_LOC (loc), (unw_fpreg_t*)val,
                                       0, c->as_arg);
  if (DWARF_IS_REG_LOC (loc))
    return (*c->as->acc.access_reg) (c->as, DWARF_GET_LOC (loc), val,
                                     0, c->as_arg);
  if (DWARF_IS_MEM_LOC (loc))
    return (*c->as->acc.access_mem) (c->as, DWARF_GET_LOC (loc), val,
                                     0, c->as_arg);
  assert(DWARF_IS_VAL_LOC (loc));
  *val = DWARF_GET_LOC (loc);
  return 0;
}

static inline int
dwarf_put (struct dwarf_cursor *c, dwarf_loc_t loc, unw_word_t val)
{
  assert(sizeof(unw_fpreg_t) == sizeof(unw_word_t));
  assert(!DWARF_IS_VAL_LOC (loc));

  if (DWARF_IS_NULL_LOC (loc))
    return -UNW_EBADREG;

  /* GPRs may be saved in FPRs */
  if (DWARF_IS_FP_LOC (loc))
    return (*c->as->acc.access_fpreg) (c->as, DWARF_GET_LOC (loc), (unw_fpreg_t*) &val,
                                       1, c->as_arg);
  if (DWARF_IS_REG_LOC (loc))
    return (*c->as->acc.access_reg) (c->as, DWARF_GET_LOC (loc), &val,
                                     1, c->as_arg);

  assert(DWARF_IS_MEM_LOC (loc));
  return (*c->as->acc.access_mem) (c->as, DWARF_GET_LOC (loc), &val,
                                   1, c->as_arg);
}

#define tdep_getcontext_trace           unw_getcontext
#define tdep_init_done                  UNW_OBJ(init_done)
#define tdep_init_mem_validate          UNW_OBJ(init_mem_validate)
#define tdep_init                       UNW_OBJ(init)
/* Platforms that support UNW_INFO_FORMAT_TABLE need to define
   tdep_search_unwind_table.  */
#define tdep_search_unwind_table        dwarf_search_unwind_table
#define tdep_find_unwind_table          dwarf_find_unwind_table
#define tdep_get_elf_image              UNW_ARCH_OBJ(get_elf_image)
#define tdep_get_exe_image_path         UNW_ARCH_OBJ(get_exe_image_path)
#define tdep_access_reg                 UNW_OBJ(access_reg)
#define tdep_access_fpreg               UNW_OBJ(access_fpreg)
#define tdep_fetch_frame(c,ip,n)        do {} while(0)
#define tdep_cache_frame(c)             0
#define tdep_reuse_frame(c,rs)          do {} while(0)
#define tdep_stash_frame(cs,rs)         do {} while(0)
#define tdep_trace(cur,addr,n)          (-UNW_ENOINFO)
#define tdep_uc_addr                    UNW_OBJ(uc_addr)

#ifdef UNW_LOCAL_ONLY
# define tdep_find_proc_info(c,ip,n)                            \
        dwarf_find_proc_info((c)->as, (ip), &(c)->pi, (n),      \
                                       (c)->as_arg)
# define tdep_put_unwind_info(as,pi,arg)                \
        dwarf_put_unwind_info((as), (pi), (arg))
#else
# define tdep_find_proc_info(c,ip,n)                                    \
        (*(c)->as->acc.find_proc_info)((c)->as, (ip), &(c)->pi, (n),    \
                                       (c)->as_arg)
# define tdep_put_unwind_info(as,pi,arg)                        \
        (*(as)->acc.put_unwind_info)((as), (pi), (arg))
#endif

#define tdep_get_as(c)                  ((c)->dwarf.as)
#define tdep_get_as_arg(c)              ((c)->dwarf.as_arg)
#define tdep_get_ip(c)                  ((c)->dwarf.ip)
#define tdep_big_endian(as)             1

extern atomic_bool tdep_init_done;

extern void tdep_init (void);
extern void tdep_init_mem_validate (void);
extern int tdep_search_unwind_table (unw_addr_space_t as, unw_word_t ip,
                                     unw_dyn_info_t *di, unw_proc_info_t *pi,
                                     int need_unwind_info, void *arg);
extern void *tdep_uc_addr (unw_tdep_context_t *uc, int reg);
extern int tdep_get_elf_image (struct elf_image *ei, pid_t pid, unw_word_t ip,
                               unsigned long *segbase, unsigned long *mapoff,
                               char *path, size_t pathlen);
extern void tdep_get_exe_image_path (char *path);
extern int tdep_access_reg (struct cursor *c, unw_regnum_t reg,
                            unw_word_t *valp, int write);
extern int tdep_access_fpreg (struct cursor *c, unw_regnum_t reg,
                              unw_fpreg_t *valp, int write);

#endif /* S390X_LIBUNWIND_I_H */
