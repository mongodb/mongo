#!/bin/sh

# this function is slight modification of the one used in RPM
# found at https://bugzilla.redhat.com/show_bug.cgi?id=834073
# written by Alexander Larsson <alexl@redhat.com>
add_minidebug()
{
  debuginfo="$1" ## we don't have separate debuginfo file
  binary="$1"

  dynsyms=`mktemp`
  funcsyms=`mktemp`
  keep_symbols=`mktemp`
  mini_debuginfo=`mktemp`

  # Extract the dynamic symbols from the main binary, there is no need to also have these
  # in the normal symbol table
  nm -D "$binary" --format=posix --defined-only | awk '{ print $1 }' | sort > "$dynsyms"
  # Extract all the text (i.e. function) symbols from the debuginfo 
  nm "$debuginfo" --format=posix --defined-only | awk '{ if ($2 == "T" || $2 == "t") print $1 }' | sort > "$funcsyms"
  # Keep all the function symbols not already in the dynamic symbol table
  comm -13 "$dynsyms" "$funcsyms" > "$keep_symbols"
  # Copy the full debuginfo, keeping only a minumal set of symbols and removing some unnecessary sections
  objcopy -S --remove-section .gdb_index --remove-section .comment --keep-symbols="$keep_symbols" "$debuginfo" "$mini_debuginfo" &> /dev/null
  #Inject the compressed data into the .gnu_debugdata section of the original binary
  xz "$mini_debuginfo"
  mini_debuginfo="${mini_debuginfo}.xz"
  objcopy --add-section .gnu_debugdata="$mini_debuginfo" "$binary"
  rm -f "$dynsyms" "$funcsyms" "$keep_symbols" "$mini_debuginfo"

  strip "$binary" ## throw away the symbol table
}


TESTDIR=`pwd`
TEMPDIR=`mktemp --tmpdir -d libunwind-test-XXXXXXXXXX`
trap "rm -r -- $TEMPDIR" EXIT

cp crasher $TEMPDIR/crasher
if [ "$1" = "-minidebuginfo" ]; then
  add_minidebug $TEMPDIR/crasher
fi

# create core dump
(
    cd $TEMPDIR
    ulimit -c 10000
    ./crasher backing_files
) 2>/dev/null
COREFILE=$TEMPDIR/core*

# magic option -testcase enables checking for the specific contents of the stack
./test-coredump-unwind $COREFILE -testcase `cat $TEMPDIR/backing_files`
