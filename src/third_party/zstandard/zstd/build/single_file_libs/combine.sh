#!/bin/sh -e

# Tool to bundle multiple C/C++ source files, inlining any includes.
# 
# Note: this POSIX-compliant script is many times slower than the original bash
# implementation (due to the grep calls) but it runs and works everywhere.
# 
# TODO: ROOTS, FOUND, etc., as arrays (since they fail on paths with spaces)
# TODO: revert to Bash-only regex (the grep ones being too slow)
# 
# Author: Carl Woffenden, Numfum GmbH (this script is released under a CC0 license/Public Domain)

# Common file roots
ROOTS="."

# -x option excluded includes
XINCS=""

# -k option includes to keep as include directives
KINCS=""

# Files previously visited
FOUND=""

# Optional destination file (empty string to write to stdout)
DESTN=""

# Whether the "#pragma once" directives should be written to the output
PONCE=0

# Prints the script usage then exits
usage() {
  echo "Usage: $0 [-r <path>] [-x <header>] [-k <header>] [-o <outfile>] infile"
  echo "  -r file root search path"
  echo "  -x file to completely exclude from inlining"
  echo "  -k file to exclude from inlining but keep the include directive"
  echo "  -p keep any '#pragma once' directives (removed by default)"
  echo "  -o output file (otherwise stdout)"
  echo "Example: $0 -r ../my/path - r ../other/path -o out.c in.c"
  exit 1
}

# Tests that the grep implementation works as expected (older OSX grep fails)
test_deps() {
  if ! echo '#include "foo"' | grep -Eq '^\s*#\s*include\s*".+"'; then
    echo "Aborting: the grep implementation fails to parse include lines"
    exit 1
  fi
  if ! echo '"foo.h"' | sed -E 's/"([^"]+)"/\1/' | grep -Eq '^foo\.h$'; then
    echo "Aborting: sed is unavailable or non-functional"
    exit 1
  fi
}

# Tests if list $1 has item $2 (returning zero on a match)
list_has_item() {
  if echo "$1" | grep -Eq "(^|\s*)$2(\$|\s*)"; then
    return 0
  else
    return 1
  fi
}

# Adds a new line with the supplied arguments to $DESTN (or stdout)
write_line() {
  if [ -n "$DESTN" ]; then
    printf '%s\n' "$@" >> "$DESTN"
  else
    printf '%s\n' "$@"
  fi
}

log_line() {
  echo $@ >&2
}

# Find this file!
resolve_include() {
  local srcdir=$1
  local inc=$2
  for root in $srcdir $ROOTS; do
    if [ -f "$root/$inc" ]; then
      # Try to reduce the file path into a canonical form (so that multiple)
      # includes of the same file are successfully deduplicated, even if they
      # are expressed differently.
      local relpath="$(realpath --relative-to . "$root/$inc" 2>/dev/null)"
      if [ "$relpath" != "" ]; then # not all realpaths support --relative-to
        echo "$relpath"
        return 0
      fi
      local relpath="$(realpath "$root/$inc" 2>/dev/null)"
      if [ "$relpath" != "" ]; then # not all distros have realpath...
        echo "$relpath"
        return 0
      fi
      # Fallback on Python to reduce the path if the above fails.
      local relpath=$(python -c "import os,sys; print os.path.relpath(sys.argv[1])" "$root/$inc" 2>/dev/null)
      if [ "$relpath" != "" ]; then # not all distros have realpath...
        echo "$relpath"
        return 0
      fi
      # Worst case, fall back to just the root + relative include path. The
      # problem with this is that it is possible to emit multiple different
      # resolved paths to the same file, depending on exactly how its included.
      # Since the main loop below keeps a list of the resolved paths it's
      # already included, in order to avoid repeated includes, this failure to
      # produce a canonical/reduced path can lead to multiple inclusions of the
      # same file. But it seems like the resulting single file library still
      # works (hurray include guards!), so I guess it's ok.
      echo "$root/$inc"
      return 0
    fi
  done
  return 1
}

# Adds the contents of $1 with any of its includes inlined
add_file() {
  local file=$1
  if [ -n "$file" ]; then
    log_line "Processing: $file"
    # Get directory of the current so we can resolve relative includes
    local srcdir="$(dirname "$file")"
    # Read the file
    local line=
    while IFS= read -r line; do
      if echo "$line" | grep -Eq '^\s*#\s*include\s*".+"'; then
        # We have an include directive so strip the (first) file
        local inc=$(echo "$line" | grep -Eo '".*"' | sed -E 's/"([^"]+)"/\1/' | head -1)
        local res_inc="$(resolve_include "$srcdir" "$inc")"
        if list_has_item "$XINCS" "$inc"; then
          # The file was excluded so error if the source attempts to use it
          write_line "#error Using excluded file: $inc"
          log_line "Excluding: $inc"
        else
          if ! list_has_item "$FOUND" "$res_inc"; then
            # The file was not previously encountered
            FOUND="$FOUND $res_inc"
            if list_has_item "$KINCS" "$inc"; then
              # But the include was flagged to keep as included
              write_line "/**** *NOT* inlining $inc ****/"
              write_line "$line"
              log_line "Not Inlining: $inc"
            else
              # The file was neither excluded nor seen before so inline it
              write_line "/**** start inlining $inc ****/"
              add_file "$res_inc"
              write_line "/**** ended inlining $inc ****/"
            fi
          else
            write_line "/**** skipping file: $inc ****/"
          fi
        fi
      else
        # Skip any 'pragma once' directives, otherwise write the source line
        local write=$PONCE
        if [ $write -eq 0 ]; then
          if echo "$line" | grep -Eqv '^\s*#\s*pragma\s*once\s*'; then
            write=1
          fi
        fi
        if [ $write -ne 0 ]; then
          write_line "$line"
        fi
      fi
    done < "$file"
  else
    write_line "#error Unable to find \"$1\""
    log_line "Error: Unable to find: \"$1\""
  fi
}

while getopts ":r:x:k:po:" opts; do
  case $opts in
  r)
    ROOTS="$ROOTS $OPTARG"
    ;;
  x)
    XINCS="$XINCS $OPTARG"
    ;;
  k)
    KINCS="$KINCS $OPTARG"
    ;;
  p)
    PONCE=1
    ;;
  o)
    DESTN="$OPTARG"
    ;;
  *)
    usage
    ;;
  esac
done
shift $((OPTIND-1))

if [ -n "$1" ]; then
  if [ -f "$1" ]; then
    if [ -n "$DESTN" ]; then
      printf "" > "$DESTN"
    fi
    test_deps
    add_file "$1"
  else
    echo "Input file not found: \"$1\""
    exit 1
  fi
else
  usage
fi
exit 0
