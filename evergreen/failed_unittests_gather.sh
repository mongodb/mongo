DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -eou pipefail

# Only run on unit test tasks so we don't target mongod binaries from cores.
if [ "${task_name}" != "run_unittests" ] && [ "${task_name}" != "run_dbtest" ]; then
  exit 0
fi

unittest_bin_dir=dist-unittests/bin
mkdir -p $unittest_bin_dir || true

# Find all core files
core_files=$(/usr/bin/find -H . \( -name "dump_*.core" -o -name "*.mdmp" \) 2> /dev/null)
for core_file in $core_files; do
  # A core file name does not always have the executable name that generated it.
  # See http://stackoverflow.com/questions/34801353/core-dump-filename-gets-thread-name-instead-of-executable-name-with-core-pattern
  # On platforms with GDB, we get the binary name from core file
  gdb=/opt/mongodbtoolchain/gdb/bin/gdb
  if [ -f $gdb ]; then
    binary_file=$($gdb -batch --quiet -ex "core $core_file" 2> /dev/null | grep "Core was generated" | cut -f2 -d "\`" | cut -f1 -d "'" | cut -f1 -d " ")
    binary_file_locations=$binary_file
  else
    # Find the base file name from the core file name, note it may be truncated.
    # Remove leading 'dump_' and trailing '.<pid>.core' or '.<pid or time>.mdmp'
    binary_file=$(echo "$core_file" | sed "s/.*\///;s/dump_//;s/\..*\.core//;s/\..*\.mdmp//")
    # Locate the binary file. Since the base file name might be truncated, the find
    # may return more than 1 file.
    binary_file_locations=$(/usr/bin/find -H . -executable -name "$binary_file*${exe}" 2> /dev/null)
  fi
  if [ -z "$binary_file_locations" ]; then
    echo "Cannot locate the unittest binary file ($binary_file) that generated the core file $core_file"
  fi
  for binary_file_location in $binary_file_locations; do
    new_binary_file=$unittest_bin_dir/$(echo "$binary_file_location" | sed "s/.*\///")
    if [ -f "$binary_file_location" ] && [ ! -f "$new_binary_file" ]; then
      cp "$binary_file_location" "$new_binary_file"
    fi

    # On Windows if a .pdb symbol file exists, include it in the archive.
    pdb_file=$(echo "$binary_file_location" | sed "s/\.exe/.pdb/")
    if [ -f "$pdb_file" ]; then
      new_pdb_file=$unittest_bin_dir/$(echo "$pdb_file" | sed "s/.*\///")
      cp "$pdb_file" "$new_pdb_file"
    fi

    # On binutils platforms, if a .debug symbol file exists, include it
    # in the archive
    debug_file=$binary_file_location.debug
    if [ -f "$debug_file" ]; then
      cp "$debug_file" "$unittest_bin_dir"
    fi

    # On macOS, these are called .dSYM and they are directories
    dsym_dir=$binary_file_location.dSYM
    if [ -d "$dsym_dir" ]; then
      cp -r "$dsym_dir" "$unittest_bin_dir"
    fi

  done
done

# Copy debug symbols for dynamic builds
lib_dir=build/install/lib
if [ -d "$lib_dir" ] && [[ -n "$core_files" ]]; then
  cp -r "$lib_dir" dist-unittests
fi
