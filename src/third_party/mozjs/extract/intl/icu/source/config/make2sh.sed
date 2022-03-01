# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
# Copyright (C) 1999-2011, International Business Machines  Corporation and others.  All Rights Reserved.
s%^\([a-zA-Z0-9\._-]*\)[ 	]*+=%\1=$(\1) %
s%^[A-Z]*_SO_TARG*%## &%
s%^SHARED_OBJECT.*%## &%
s@^_%.*@## &@
s%^LD_SONAME.*%## &%
s%$(\([^\)]*\))%${\1}%g
s%^	%#M#	%
s@^[a-zA-Z%$.][^=]*$@#M#&@
s@^\([a-zA-Z][-.a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1="\2"@
s@^\([a-zA-Z][-a-zA-Z_0-9-]*\)\.\([a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1_\2=\3@
s@^\([a-zA-Z][-a-zA-Z_0-9-]*\)\-\([a-zA-Z_0-9-]*\)[	 ]*=[ 	]*\(.*\)@\1_\2=\3@
s@\${\([a-zA-Z][-a-zA-Z_0-9-]*\)\.\([a-zA-Z_0-9-]*\)}@${\1_\2}@g
s@^\(prefix\)=\(.*\)@default_\1=\2\
if [ "x${\1}" = "x" ]; then \1="$default_\1"; fi@
s@^\(ENABLE_RPATH\)=\(.*\)@default_\1=\2\
if [ "x${\1}" = "x" ]; then \1="$default_\1"; fi@
s%^#SH#[ ]*%%
s%'\$\$'%\\\$\\\$%g
