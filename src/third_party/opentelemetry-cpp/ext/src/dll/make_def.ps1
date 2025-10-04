# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

param ($dumpbin, $bindir, $inputfile, $targetfile)
   
 "LIBRARY opentelemetry_cpp`r`nEXPORTS`r`n" > $targetfile
    
 Get-ChildItem -Verbose -Path $bindir/sdk/*,$bindir/exporters/* -Include *.lib -Recurse | % { & "$dumpbin" /SYMBOLS $_ | Select-String -Pattern @(Get-Content -Verbose -Path "$inputfile" | Where-Object { $_.Trim() -ne '' } | % { "External\s+\|\s+(\?+[0-9]?$_[^\s]*)\s+\((.*)\)$" }) | % { "; $($_.matches.groups[2])`r`n$($_.matches.groups[1])" } >> $targetfile }
