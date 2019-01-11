# Spidermonkey JSAPI rooting analysis

This directory contains scripts for running Brian Hackett's static GC rooting
and thread heap write safety analyses on a JS source directory.

The easiest way to get this running is to not try to do the instrumented
compilation locally. Instead, grab the relevant files from a try server push
and analyze them locally.

Local Analysis of Downloaded Intermediate Files

1. Do a try push with "--upload-xdbs" appended to the try: ..." line.

2. Create an empty directory to run the analysis.

3. When the try job is complete, download the resulting src_body.xdb.bz2, src_comp.xdb.bz2,
and file_source.xdb.bz2 files into your directory.

4. Build an optimized JS shell with ctypes. Note that this does not need to
match the source you are analyzing in any way; in fact, you pretty much never
need to update this once you've built it. (Though I reserve the right to use
any new JS features implemented in Spidermonkey in the future...)

    mkdir <objdir>
    cd <objdir>
    <srcpath>/js/src/configure --disable-debug --enable-optimize --enable-ctypes
    make -j6 -s

5. Clone and build sixgill:

    hg clone https://hg.mozilla.org/users/sfink_mozilla.com/sixgill
    cd sixgill
    ./release.sh --build

If you are on osx, the sixgill build will fail horribly. Let it, then do

    make bin/xdb.so CXX=clang++

6. Make a defaults.py file containing the following, with your own paths filled in:

    js = "<objdir>/dist/bin/js"
    sixgill_bin = "<sixgill-dir>/bin"

7a. For the rooting analysis, run

    python <srcdir>/js/src/devtools/rootAnalysis/analyze.py gcTypes

7b. For the heap write analysis, run

    python <srcdir>/js/src/devtools/rootAnalysis/analyze.py heapwrites

Also, you may wish to run with -v (aka --verbose) to see the exact commands
executed that you can cut & paste if needed. (I use them to run under the JS
debugger when I'm working on the analysis.)

----

Or if you *do* want to run the full analysis locally, then you may face the
dragons. To use it on SpiderMonkey:

1.  Be on Fedora/CentOS/RedHat Linux x86_64, or a Docker image of one of those.

    Specifically, the prebuilt GCC **won't work on Ubuntu**
    without the `CFLAGS` and `CXXFLAGS` settings from
    <http://trac.wildfiregames.com/wiki/StaticRootingAnalysis>.

2.  Have the Gecko build prerequisites installed.

3.  Install taskcluster-vcs, eg by doing

        npm install taskcluster-vcs
        export PATH="$PATH:$(pwd)/node_modules/.bin"

4. In some directory, using $SRCDIR as the top of your Gecko source checkout,
    run these commands:

        mkdir work
        cd work
        ( export GECKO_DIR=$SRCDIR; $GECKO_DIR/taskcluster/scripts/builder/build-haz-linux.sh $(pwd) --dep )

The `--dep` is optional, and will avoid rebuilding the JS shell used to run the
analysis later.

If you see the error ``/lib/../lib64/crti.o: unrecognized relocation (0x2a) in section .init`` then have a version mismatch between the precompiled gcc used in automation and your installed glibc. The easiest way to fix this is to delete the ld provided with the precompiled gcc (it will be in two places, one given in the first part of the error message), which will cause gcc to fall back to your system ld. But you will need to additionally pass ``--no-tooltool`` to build-haz-linux.sh. With the current package, you could do the deletion with

    rm gcc/bin/ld
    rm gcc/x86_64-unknown-linux-gnu/bin/ld

Output goes to `analysis/hazards.txt`. This will run the
analysis on the js/src tree only; if you wish to analyze the full browser, use

    ( export GECKO_DIR=$SRCDIR; $GECKO_DIR/taskcluster/scripts/builder/build-haz-linux.sh --project browser $(pwd) )

After running the analysis once, you can reuse the `*.xdb` database files
generated, using modified analysis scripts, by running
`analysis/run-analysis.sh` (or pass `--list` to see ways to select even more
restrictive parts of the overall analysis; the default is `gcTypes` which will
do everything but regenerate the xdb files).

Also, you can pass `-v` to get exact command lines to cut & paste for running the
various stages, which is helpful for running under a debugger.


## Overview of what is going on here

So what does this actually do?

1.  It downloads a GCC compiler and plugin ("sixgill") from Mozilla servers, using
    "tooltool" (a binary archive tool).

2. It runs `run_complete`, a script that builds the target codebase with the
    downloaded GCC, generating a few database files containing control flow
    graphs of the full compile, along with type information etc.

3.  Then it runs `analyze.py`, a Python script, which runs all the scripts
    which actually perform the analysis -- the tricky parts.
    (Those scripts are written in JS.)
