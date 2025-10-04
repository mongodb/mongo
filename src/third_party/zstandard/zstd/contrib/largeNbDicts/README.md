largeNbDicts
=====================

`largeNbDicts` is a benchmark test tool
dedicated to the specific scenario of
dictionary decompression using a very large number of dictionaries.
When dictionaries are constantly changing, they are always "cold",
suffering from increased latency due to cache misses.

The tool is created in a bid to investigate performance for this scenario,
and experiment mitigation techniques.

Command line :
```
largeNbDicts [Options] filename(s)

Options : 
-z          : benchmark compression (default) 
-d          : benchmark decompression 
-r          : recursively load all files in subdirectories (default: off) 
-B#         : split input into blocks of size # (default: no split) 
-#          : use compression level # (default: 3) 
-D #        : use # as a dictionary (default: create one) 
-i#         : nb benchmark rounds (default: 6) 
--nbBlocks=#: use # blocks for bench (default: one per file) 
--nbDicts=# : create # dictionaries for bench (default: one per block) 
-h          : help (this text) 
 
Advanced Options (see zstd.h for documentation) : 
--dedicated-dict-search
--dict-content-type=#
--dict-attach-pref=#
```
