/* libunwind - a platform-independent unwind library
   Copyright (C) 2002 Hewlett-Packard Co
   Copyright (C) 2007 David Mosberger-Tang
        Contributed by David Mosberger-Tang <dmosberger@gmail.com>

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

#include "libunwind_i.h"
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(HAVE_SYS_SYSCALL_H)
# include <sys/syscall.h>
#endif
#include <stdatomic.h>

#include "unwind_i.h"

#ifdef UNW_REMOTE_ONLY

/* unw_local_addr_space is a NULL pointer in this case.  */
unw_addr_space_t unw_local_addr_space;

#else /* !UNW_REMOTE_ONLY */

static struct unw_addr_space local_addr_space;

unw_addr_space_t unw_local_addr_space = &local_addr_space;

static void
put_unwind_info (unw_addr_space_t as UNUSED, unw_proc_info_t *proc_info UNUSED, void *arg UNUSED)
{
  /* it's a no-op */
}

static int
get_dyn_info_list_addr (unw_addr_space_t as UNUSED, unw_word_t *dyn_info_list_addr,
                        void *arg UNUSED)
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
access_mem (unw_addr_space_t as UNUSED, unw_word_t addr, unw_word_t *val, int write,
            void *arg)
{
  if (unlikely (write))
    {
      Debug (16, "mem[%016lx] <- %lx\n", addr, *val);
      memcpy ((void *) addr, val, sizeof(unw_word_t));
    }
  else
    {
      /* validate address */
      if (unlikely (AS_ARG_GET_VALIDATE(arg))
          && unlikely (!unw_address_is_valid (addr, sizeof (unw_word_t)))) {
        Debug (16, "mem[%016lx] -> invalid\n", addr);
        return -1;
      }
      memcpy (val, (void *) addr, sizeof(unw_word_t));
      Debug (16, "mem[%016lx] -> %lx\n", addr, *val);
    }
  return 0;
}

static int
access_reg (unw_addr_space_t as UNUSED, unw_regnum_t reg, unw_word_t *val, int write,
            void *arg)
{
  unw_word_t *addr;
  ucontext_t *uc = AS_ARG_GET_UC_PTR(arg);

  if (unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      memcpy ((void *) addr, val, sizeof(unw_word_t));
      Debug (12, "%s <- 0x%016lx\n", unw_regname (reg), *val);
    }
  else
    {
      memcpy (val, (void *) addr, sizeof(unw_word_t));
      Debug (12, "%s -> 0x%016lx\n", unw_regname (reg), *val);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  return -UNW_EBADREG;
}

static int
access_fpreg (unw_addr_space_t as UNUSED, unw_regnum_t reg, unw_fpreg_t *val,
              int write, void *arg)
{
  ucontext_t *uc = AS_ARG_GET_UC_PTR(arg);
  unw_fpreg_t *addr;

  if (!unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      Debug (12, "%s <- %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
      *(unw_fpreg_t *) addr = *val;
    }
  else
    {
      *val = *(unw_fpreg_t *) addr;
      Debug (12, "%s -> %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  /* attempt to access a non-preserved register */
  return -UNW_EBADREG;
}

static int
get_static_proc_name (unw_addr_space_t as, unw_word_t ip,
                      char *buf, size_t buf_len, unw_word_t *offp,
                      void *arg UNUSED)
{
  return _Uelf64_get_proc_name (as, getpid (), ip, buf, buf_len, offp);
}

static int
get_static_elf_filename (unw_addr_space_t as, unw_word_t ip,
                         char *buf, size_t buf_len, unw_word_t *offp,
                         void *arg UNUSED)
{
  return _Uelf64_get_elf_filename (as, getpid (), ip, buf, buf_len, offp);
}

HIDDEN void
x86_64_local_addr_space_init (void)
{
  memset (&local_addr_space, 0, sizeof (local_addr_space));
#ifndef UNW_REMOTE_ONLY
# if defined(HAVE_DL_ITERATE_PHDR)
  local_addr_space.iterate_phdr_function = dl_iterate_phdr;
# endif
#endif
  local_addr_space.caching_policy = UNWI_DEFAULT_CACHING_POLICY;
  local_addr_space.acc.find_proc_info = dwarf_find_proc_info;
  local_addr_space.acc.put_unwind_info = put_unwind_info;
  local_addr_space.acc.get_dyn_info_list_addr = get_dyn_info_list_addr;
  local_addr_space.acc.access_mem = access_mem;
  local_addr_space.acc.access_reg = access_reg;
  local_addr_space.acc.access_fpreg = access_fpreg;
  local_addr_space.acc.resume = x86_64_local_resume;
  local_addr_space.acc.get_proc_name = get_static_proc_name;
  local_addr_space.acc.get_elf_filename = get_static_elf_filename;
  unw_flush_cache (&local_addr_space, 0, 0);
}

#endif /* !UNW_REMOTE_ONLY */
