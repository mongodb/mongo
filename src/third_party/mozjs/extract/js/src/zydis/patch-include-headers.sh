#!/bin/sh
for fn in `find . -name '*.[hc]'`
do
    gawk '
{ if (match($0, /( *# *include +)<(Zy.*ExportConfig.h|Zy(dis|core).*\.h)>/, res)) {
    print res[1] "\"zydis/" res[2] "\""
    next
  } else if (match($0, /( *# *include +)<(Generated\/.*\.inc)>/, res)) {
    print res[1] "\"zydis/Zydis/" res[2] "\""
    next
  }
  print $0 }' $fn > $fn.bak
    mv $fn.bak $fn
done
