/* libunwind - a platform-independent unwind library
   Copyright (C) 2008 CodeSourcery
   Copyright (C) 2021 Loongson Technology Corporation Limited

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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>

#include "unwind_i.h"

#ifdef UNW_REMOTE_ONLY

/* unw_local_addr_space is a NULL pointer in this case.  */
unw_addr_space_t unw_local_addr_space;

#else /* !UNW_REMOTE_ONLY */

static struct unw_addr_space local_addr_space;

unw_addr_space_t unw_local_addr_space = &local_addr_space;

static inline void *
uc_addr (ucontext_t *uc, int reg)
{
  if (reg >= UNW_LOONGARCH64_R0 && reg <= UNW_LOONGARCH64_R31)
    return &uc->uc_mcontext.__gregs[reg - UNW_LOONGARCH64_R0];
  else if (reg == UNW_LOONGARCH64_PC)
    return &uc->uc_mcontext.__pc;
  else
    return NULL;
}

# ifdef UNW_LOCAL_ONLY

HIDDEN void *
tdep_uc_addr (ucontext_t *uc, int reg)
{
  return uc_addr (uc, reg);
}

# endif /* UNW_LOCAL_ONLY */

static void
put_unwind_info (unw_addr_space_t as, unw_proc_info_t *proc_info, void *arg)
{
  /* it's a no-op */
}

static int
get_dyn_info_list_addr (unw_addr_space_t as, unw_word_t *dyn_info_list_addr,
                        void *arg)
{
#ifndef UNW_LOCAL_ONLY
# pragma weak _U_dyn_info_list_addr
  if (!_U_dyn_info_list_addr)
    return -UNW_ENOINFO;
#endif
  // Access the `_U_dyn_info_list` from `LOCAL_ONLY` library, i.e. libunwind.so.
  *dyn_info_list_addr = _U_dyn_info_list_addr ();
  return 0;
}


static int
access_mem (unw_addr_space_t as, unw_word_t addr, unw_word_t *val, int write,
            void *arg)
{
  if (unlikely (write))
    {
      Debug (16, "mem[%llx] <- %llx\n", (long long) addr, (long long) *val);
      *(unw_word_t *) (intptr_t) addr = *val;
    }
  else
    {
      /* validate address */
      const struct cursor *c = (const struct cursor *)arg;
      if (likely (c != NULL) && unlikely (c->validate)
          && unlikely (!unw_address_is_valid (addr, sizeof(unw_word_t)))) {
        Debug (16, "mem[%016lx] -> invalid\n", addr);
        return -1;
      }
      *val = *(unw_word_t *) (intptr_t) addr;
      Debug (16, "mem[%llx] -> %llx\n", (long long) addr, (long long) *val);
    }
  return 0;
}

static int
access_reg (unw_addr_space_t as, unw_regnum_t reg, unw_word_t *val, int write,
            void *arg)
{
  unw_word_t *addr;
  ucontext_t *uc = ((struct cursor *)arg)->uc;

  if (unw_is_fpreg (reg))
    goto badreg;

  Debug (16, "reg = %s\n", unw_regname (reg));
  if (!(addr = uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      *(unw_word_t *) (intptr_t) addr = (unw_word_t) *val;
      Debug (12, "%s <- %llx\n", unw_regname (reg), (long long) *val);
    }
  else
    {
      *val = (unw_word_t) *(unw_word_t *) (intptr_t) addr;
      Debug (12, "%s -> %llx\n", unw_regname (reg), (long long) *val);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  return -UNW_EBADREG;
}

static int
access_fpreg (unw_addr_space_t as, unw_regnum_t reg, unw_fpreg_t *val,
              int write, void *arg)
{
  return 0;
}

static int
get_static_proc_name (unw_addr_space_t as, unw_word_t ip,
                      char *buf, size_t buf_len, unw_word_t *offp,
                      void *arg)
{
  return elf_w (get_proc_name) (as, getpid (), ip, buf, buf_len, offp);
}

static int
get_static_elf_filename (unw_addr_space_t as, unw_word_t ip,
                         char *buf, size_t buf_len, unw_word_t *offp,
                         void *arg)
{
  return elf_w (get_elf_filename) (as, getpid (), ip, buf, buf_len, offp);
}

HIDDEN void
loongarch64_local_addr_space_init (void)
{
  memset (&local_addr_space, 0, sizeof (local_addr_space));

#ifndef UNW_REMOTE_ONLY
# if defined(HAVE_DL_ITERATE_PHDR)
  local_addr_space.iterate_phdr_function = dl_iterate_phdr;
# endif
#endif
  local_addr_space.caching_policy = UNW_CACHE_GLOBAL;
  local_addr_space.acc.find_proc_info = dwarf_find_proc_info;
  local_addr_space.acc.put_unwind_info = put_unwind_info;
  local_addr_space.acc.get_dyn_info_list_addr = get_dyn_info_list_addr;
  local_addr_space.acc.access_mem = access_mem;
  local_addr_space.acc.access_reg = access_reg;
  local_addr_space.acc.access_fpreg = access_fpreg;
  local_addr_space.acc.resume = loongarch64_local_resume;
  local_addr_space.acc.get_proc_name = get_static_proc_name;
  local_addr_space.acc.get_elf_filename = get_static_elf_filename;
  unw_flush_cache (&local_addr_space, 0, 0);
}

#endif /* !UNW_REMOTE_ONLY */
