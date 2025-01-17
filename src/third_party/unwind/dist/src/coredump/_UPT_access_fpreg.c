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
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "_UCD_lib.h"
#include "_UCD_internal.h"

int
_UCD_access_fpreg (unw_addr_space_t  as UNUSED,
                   unw_regnum_t      reg UNUSED,
                   unw_fpreg_t      *val UNUSED,
                   int               write,
                   void             *arg)
{
  struct UCD_info *ui UNUSED = arg;

  if (write)
    {
      Debug(0, "write is not supported\n");
      return -UNW_EINVAL;
    }

#ifdef __s390x__
  if (reg < UNW_S390X_F0 || reg > UNW_S390X_F15)
    {
      Debug(0, "bad regnum:%d\n", reg);
      return -UNW_EINVAL;
    }

  *val = ui->fpregset->fprs[reg - UNW_S390X_F0].d;
  return 0;
#else
  print_error (__func__);
  print_error (" not implemented for this architecture\n");
  return -UNW_EINVAL;
#endif
}
