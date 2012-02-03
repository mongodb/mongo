dnl AC_CONFIG_FILES conditionalization requires using AM_COND_IF, however 
dnl AM_COND_IF is new to Automake 1.11.  To use it on new Automake without 
dnl requiring same, a fallback implementation for older Autoconf is provided. 
dnl Note that disabling of AC_CONFIG_FILES requires Automake 1.11, this code 
dnl is correct only in terms of m4sh generated script. 
m4_ifndef([AM_COND_IF], [AC_DEFUN([AM_COND_IF], [ 
if test -z "$$1_TRUE"; then : 
  m4_n([$2])[]dnl 
m4_ifval([$3], 
[else 
  $3 
])dnl 
fi[]dnl 
])]) 
