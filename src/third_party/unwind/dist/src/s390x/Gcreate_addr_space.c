/* libunwind - a platform-independent unwind library
   Copyright (C) 2003 Hewlett-Packard Co
        Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>
   Copyright (C) 2012 Tommi Rantala <tt.rantala@gmail.com>
   Modified for s390x by Michael Munday <mike.munday@ibm.com>

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

#include <stdlib.h>

#include "unwind_i.h"

unw_addr_space_t
unw_create_addr_space (unw_accessors_t *a, int byte_order)
{
#ifdef UNW_LOCAL_ONLY
  return NULL;
#else
  unw_addr_space_t as;

  /*
   * s390x supports only big-endian.
   */
  if (byte_order != 0 && byte_order != UNW_BIG_ENDIAN)
    return NULL;

  as = malloc (sizeof (*as));
  if (!as)
    return NULL;

  memset (as, 0, sizeof (*as));

  as->acc = *a;

  return as;
#endif
}
