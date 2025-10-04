/*
 * Copyright 2020, 2022 Blackberry Limited.
 *
 * This file is part of libunwind.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "unw_nto_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/elf.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <unistd.h>


/**
 * Locate info needed to unwind a particular procedure.
 * @param[in]  as                The address space object
 * @param[in]  ip                Address of an instruction inside the procedure.
 * @param[out] pi                Pointer to where the information should be written.
 * @param[in]  need_unwind_info  Flags is certain fields are mandatory i *pi:
 *                                 - format
 *                                 - unwind_info_size
 *                                 - unwind_info
 * @param[in]  arg               The NTO unwind context.
 *
 * @returns 0 on normal, successful completion and @c -UNW_ESTOPUNWIND to signal
 * the end of the frame-chain.  Returns @c -UNW_ENOINFO otherwise indicating an
 * error.
 *
 * This function is part of the public API of the libunwind API library and is a
 * callback passed to @c unw_create_addr_space() through the @c unw_accessors_t
 * object.
 */
int unw_nto_find_proc_info (unw_addr_space_t as,
                            unw_word_t       ip,
                            unw_proc_info_t *pi,
                            int              need_unwind_info,
                            void            *arg)
{
  unw_nto_internal_t *uni = (unw_nto_internal_t *)arg;
  int ret = -UNW_ENOINFO;
  unsigned long segbase = 0;
  unsigned long mapoff = 0;
  char path[PATH_MAX];
  invalidate_edi (&uni->edi);
  ret = tdep_get_elf_image (&uni->edi.ei,
                            uni->pid,
                            ip,
                            &segbase,
                            &mapoff,
                            path,
                            sizeof (path));

  if (ret >= 0)
    {
      if (tdep_find_unwind_table (&uni->edi, as, path, segbase, mapoff, ip) >= 0)
        {
          if (uni->edi.di_cache.format != -1)
            {
              ret = tdep_search_unwind_table (as, ip, &uni->edi.di_cache,
                                              pi, need_unwind_info, uni);
            }

          if (ret == -UNW_ENOINFO && uni->edi.di_debug.format != -1)
            {
              ret = tdep_search_unwind_table (as, ip, &uni->edi.di_debug, pi,
                                              need_unwind_info, uni);
            }
        }
    }

  return ret;
}

