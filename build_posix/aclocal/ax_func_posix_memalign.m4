# ===========================================================================
#  http://www.gnu.org/software/autoconf-archive/ax_func_posix_memalign.html
# ===========================================================================
#
# SYNOPSIS
#
#   AX_FUNC_POSIX_MEMALIGN
#
# DESCRIPTION
#
#   Some versions of posix_memalign (notably glibc 2.2.5) incorrectly apply
#   their power-of-two check to the size argument, not the alignment
#   argument. AX_FUNC_POSIX_MEMALIGN defines HAVE_POSIX_MEMALIGN if the
#   power-of-two check is correctly applied to the alignment argument.
#
# LICENSE
#
#   Copyright (c) 2008 Scott Pakin <pakin@uiuc.edu>
#
#   Copying and distribution of this file, with or without modification, are
#   permitted in any medium without royalty provided the copyright notice
#   and this notice are preserved. This file is offered as-is, without any
#   warranty.

#serial 7

AC_DEFUN([AX_FUNC_POSIX_MEMALIGN],
[AC_CACHE_CHECK([for working posix_memalign],
  [ax_cv_func_posix_memalign_works],
  [AC_RUN_IFELSE([AC_LANG_SOURCE([[
#include <stdlib.h>

int
main ()
{
  void *buffer;

  /* Some versions of glibc incorrectly perform the alignment check on
   * the size word. */
  exit (posix_memalign (&buffer, sizeof(void *), 123) != 0);
}
    ]])],
    [ax_cv_func_posix_memalign_works=yes],
    [ax_cv_func_posix_memalign_works=no],
    [ax_cv_func_posix_memalign_works=no])])
if test "$ax_cv_func_posix_memalign_works" = "yes" ; then
  AC_DEFINE([HAVE_POSIX_MEMALIGN], [1],
    [Define to 1 if `posix_memalign' works.])
fi
])
