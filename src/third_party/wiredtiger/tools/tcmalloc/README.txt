WiredTiger utilizes tcmalloc in testing through LD_PRELOAD.

Implicit
========

To load tcmalloc into your current environment by running in the root
of your workspace:

export LD_PRELOAD=$PWD/TCMALLOC_LIB/libtcmalloc.so

This will be in effect until you exit the current shell.

NOTE: This will affect ANY binary that you run in that environment:
this includes system commands.

Explicit
========

Alternatively you can source the script define-with_tcmalloc.sh in
this directory, like this:

source tools/tcmalloc/define-with_tcmalloc.sh

Which defines a shell function "with_tcmalloc" in the current
environment that you can prefix commands with to define LD_PRELOAD for
that invocation ONLY.

For example:

 $ with_tcmalloc ./wt -h

Which is equvialent to:

 $ LD_PRELOAD=$PWD/libtcmalloc.so ./wt -h

Debugging
=========

For debugging in gdb you can use $PWD/TCMALLOC_LIB/tcmalloc.gdb

On the command line:

 $ gdb -x $PWD/TCMALLOC_LIB/tcmalloc.gdb wt

Or from within gdb:

 (gdb) source $PWD/TCMALLOC_LIB/tcmalloc.gdb
