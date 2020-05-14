#!/bin/sh -e

# Tool to bundle multiple C/C++ source files, inlining any includes.
# 
# Note: this POSIX-compliant script is many times slower than the original bash
# implementation (due to the grep calls) but it runs and works everywhere.
# 
# TODO: ROOTS and FOUND as arrays (since they fail on paths with spaces)
# 
# Author: Carl Woffenden, Numfum GmbH (released under a CC0 license)

# Common file roots
ROOTS="./"

# Files previously visited
FOUND=""

# Optional destination file (empty string to write to stdout)
DESTN=""

# Prints the script usage then exits
usage() {
  echo "Usage: $0 [-r <path>] [-o <outfile>] infile"
  echo "  -r file root search paths"
  echo "  -o output file (otherwise stdout)"
  echo "Example: $0 -r ../my/path - r ../other/path -o out.c in.c"
  exit 1
}

# Tests that the grep implementation works as expected (older OSX grep fails)
test_grep() {
	if ! echo '#include "foo"' | grep -Eq '^\s*#\s*include\s*".+"'; then
		echo "Aborting: the grep implementation fails to parse include lines"
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

# Adds the contents of $1 with any of its includes inlined
add_file() {
  # Match the path
  local file=
  if [ -f "$1" ]; then
    file="$1"
  else
    for root in $ROOTS; do
      if test -f "$root/$1"; then
        file="$root/$1"
      fi
    done
  fi
  if [ -n "$file" ]; then
    # Read the file
    local line=
    while IFS= read -r line; do
      if echo "$line" | grep -Eq '^\s*#\s*include\s*".+"'; then
        # We have an include directive so strip the (first) file
        local inc=$(echo "$line" | grep -Eo '".*"' | grep -Eo '\w*(\.?\w+)+' | head -1)
        if ! list_has_item "$FOUND" "$inc"; then
          # And we've not previously encountered it
          FOUND="$FOUND $inc"
          write_line "/**** start inlining $inc ****/"
          add_file "$inc"
          write_line "/**** ended inlining $inc ****/"
        else
          write_line "/**** skipping file: $inc ****/"
        fi
      else
        # Otherwise write the source line
        write_line "$line"
      fi
    done < "$file"
  else
    write_line "#error Unable to find \"$1\""
  fi
}

while getopts ":r:o:" opts; do
  case $opts in
  r)
    ROOTS="$OPTARG $ROOTS"
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
    test_grep
    add_file $1
  else
    echo "Input file not found: \"$1\""
    exit 1
  fi
else
  usage
fi
exit 0
