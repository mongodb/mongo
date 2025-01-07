/* libunwind - a platform-independent unwind library
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
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include "libunwind_i.h"
#include "remote.h"

int
unw_get_elf_filename_by_ip (unw_addr_space_t as, unw_word_t ip,
                            char *buf, size_t buf_len, unw_word_t *offp,
                            void *arg)
{
  unw_accessors_t *a = unw_get_accessors_int (as);
  unw_proc_info_t pi;
  int ret;

  buf[0] = '\0';        /* always return a valid string, even if it's empty */

  ret = unwi_find_dynamic_proc_info (as, ip, &pi, 1, arg);
  if (ret == 0)
    {
      unwi_put_dynamic_unwind_info (as, &pi, arg);
      return -UNW_ENOINFO;
    }

  if (a->get_elf_filename)
    return (*a->get_elf_filename) (as, ip, buf, buf_len, offp, arg);

  return -UNW_ENOINFO;
}

int
unw_get_elf_filename (unw_cursor_t *cursor, char *buf, size_t buf_len,
                   unw_word_t *offp)
{
  struct cursor *c = (struct cursor *) cursor;
  unw_word_t ip;
  int error;

  ip = tdep_get_ip (c);
#if !defined(__ia64__)
  if (c->dwarf.use_prev_instr)
    {
#if defined(__arm__)
      /* On arm, the least bit denotes thumb/arm mode, clear it. */
      ip &= ~(unw_word_t)0x1;
#endif
      --ip;
    }

#endif
  error = unw_get_elf_filename_by_ip (tdep_get_as (c), ip, buf, buf_len, offp,
                                      tdep_get_as_arg (c));
#if !defined(__ia64__)
  if (c->dwarf.use_prev_instr && offp != NULL && error == 0)
    *offp += 1;
#endif
  return error;
}
