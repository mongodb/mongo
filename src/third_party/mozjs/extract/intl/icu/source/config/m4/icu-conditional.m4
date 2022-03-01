# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
# Copyright (c) 1999-2015, International Business Machines Corporation and
# others. All Rights Reserved.

# moved here from ../../acinclude.m4

# ICU_CONDITIONAL - similar example taken from Automake 1.4
AC_DEFUN([ICU_CONDITIONAL],
[AC_SUBST($1_TRUE)
AC_SUBST(U_HAVE_$1)
if $2; then
  $1_TRUE=
  U_HAVE_$1=1
else
  $1_TRUE='#'
  U_HAVE_$1=0
fi])
