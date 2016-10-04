# Spidermonkey JSAPI rooting analysis

This directory contains scripts and a makefile for running Brian Hackett's
static GC rooting analysis on a JS source directory.

To use it on SpiderMonkey:

1.  Be on Fedora/CentOS/RedHat Linux x86_64.

    (Specifically, the prebuilt GCC **won't work on Ubuntu**
    without the `CFLAGS` and `CXXFLAGS` settings from
    http://trac.wildfiregames.com/wiki/StaticRootingAnalysis .)

2.  Have the Gecko build prerequisites installed.

3.  In this directory, run these commands.

        mkdir builddir
        cd builddir
        ../run-analysis.sh

`run-analysis.sh` is kind of like `configure` and `make` combined:
the build directory can be wherever you want
and you can name it whatever you want.
(You could just run it right here in the source tree, and it would work,
but don't do that -- it spits out files all over the place and
then you'd have to clean up your source tree later.)

Output goes to `hazards.txt` in the builddir.

To use this analysis on any other codebase,
make a copy of `run-analysis.sh` and adapt it for your code.


## Overview of what is going on here

So what does `run-analysis.sh` actually do?

1.  **It insecurely downloads software over HTTP.** Yeah.
    See `run-analysis.sh` for details.

2.  It runs `run_complete`, a Perl script, which builds the target
    codebase with a custom hacked GCC, generating a few database files
    containing (among other data) the full call graph.

3.  Then it runs `analyze.py`, a Python script, which runs all the scripts
    which actually perform the analysis -- the tricky parts.
    (Those scripts are written in JS.)

