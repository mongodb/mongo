dnl
dnl Local autoconf macros used with mozilla
dnl The contents of this file are under the Public Domain.
dnl

builtin(include, ../../build/autoconf/hooks.m4)dnl
builtin(include, ../../build/autoconf/config.status.m4)dnl
builtin(include, ../../build/autoconf/toolchain.m4)dnl
builtin(include, ../../build/autoconf/altoptions.m4)dnl
builtin(include, ../../build/autoconf/mozprog.m4)dnl
builtin(include, ../../build/autoconf/mozheader.m4)dnl
builtin(include, ../../build/autoconf/compiler-opts.m4)dnl
builtin(include, ../../build/autoconf/arch.m4)dnl
builtin(include, ../../build/autoconf/clang-plugin.m4)dnl
builtin(include, ../../build/autoconf/sanitize.m4)dnl

define([__MOZ_AC_INIT_PREPARE], defn([AC_INIT_PREPARE]))
define([AC_INIT_PREPARE],
[if test -z "$srcdir"; then
  srcdir=`dirname "[$]0"`
fi
srcdir="$srcdir/../.."
__MOZ_AC_INIT_PREPARE($1)
])

MOZ_PROG_CHECKMSYS()
dnl This won't actually read the mozconfig, but data that configure.py
dnl will have placed for us to read. Configure.py takes care of not reading
dnl the mozconfig where appropriate but can still give us some variables
dnl to read.
MOZ_READ_MOZCONFIG(.)
