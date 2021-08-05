#!/bin/bash
set +o errexit

if ! command -v shfmt &>/dev/null; then
  echo "Could not find the 'shfmt' command"
  echo ""
  echo "Install via"
  echo ""
  echo "    brew install shfmt"
  echo ""
  exit 1
fi

lint_dirs="evergreen"

if [ "$1" = "fix" ]; then
  shfmt -w -i 2 -bn -sr "$lint_dirs"
fi

output_file="shfmt_output.txt"
exit_code=0

shfmt -d -i 2 -bn -sr "$lint_dirs" >"$output_file"
if [ -s "$output_file" ]; then
  echo "ERROR: Found formatting errors in shell script files in directories: $lint_dirs"
  echo ""
  cat "$output_file"
  echo ""
  echo "To fix formatting errors run"
  echo ""
  echo "    ./buildscripts/shellscripts-linters.sh fix"
  echo ""
  exit_code=1
fi
rm -rf "$output_file"

exit "$exit_code"
