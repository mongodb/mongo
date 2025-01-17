/*
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

#include <limits.h>
#include <stdio.h>

int unw_nto_get_elf_filename (unw_addr_space_t  as,
                              unw_word_t        ip,
                              char             *buf,
                              size_t            buf_len,
                              unw_word_t       *offp,
                              void             *arg)
{
  unw_nto_internal_t *uni = (unw_nto_internal_t *)arg;
  char path[PATH_MAX] = {0};
  size_t path_len = sizeof (path);
  int ret = -UNW_ENOINFO;
#if UNW_ELF_CLASS == UNW_ELFCLASS64
  ret = _Uelf64_get_elf_filename (as, uni->pid, ip, path, path_len, offp);
#elif UNW_ELF_CLASS == UNW_ELFCLASS32
  ret = _Uelf32_get_elf_filename (as, uni->pid, ip, path, path_len, offp);
#else
# error no valid ELF class defined
#endif

  if (ret >= 0)
      snprintf (buf, buf_len, "%s", path);

  return ret;
}
