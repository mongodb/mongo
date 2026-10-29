cd src
# Find all core files and symlink them to src
# -H is used to follow hard-links, but add bazel-testlogs explicitly. This ensures we look
# in bazel-testlogs, but don't follow soft-links and end up with multiple copies of the same
# core dump from bazel-testlogs, bazel-out, etc.
core_files=$(/usr/bin/find -H .. bazel-testlogs \( -name "*.core" -o -name "*.mdmp" \) 2>/dev/null)
for core_file in $core_files; do
    base_name=$(echo $core_file | sed "s/.*\///")
    # Symlink file if it does not already exist
    if [ ! -f $base_name ]; then
        ln -sf $core_file $base_name
    fi
done

# Collect the boring core dump PID files alongside the core dumps. resmoke writes this file
# to its working directory, which is src/ for a plain resmoke task but the test's undeclared
# outputs directory under Bazel. gen_hang_analyzer_tasks.py's non-Bazel generator reads it
# relative to src/, so merge every copy we can find into src/ or all the core dumps gathered
# above look interesting and we generate an unnecessary core analysis task.
boring_pids_file="boring_core_dumps.txt"
boring_pids_dest=$(pwd)/$boring_pids_file
boring_pids_files=$(/usr/bin/find -H .. bazel-testlogs -name "$boring_pids_file" 2>/dev/null)
for found in $boring_pids_files; do
    # Don't append the destination to itself.
    if [ "$(realpath "$found" 2>/dev/null)" = "$(realpath "$boring_pids_dest" 2>/dev/null)" ]; then
        continue
    fi
    cat "$found" >>"$boring_pids_file"
done
if [ -f "$boring_pids_file" ]; then
    sort -u "$boring_pids_file" -o "$boring_pids_file"
fi

# Find all gzipped core files and decompress them to src
gzipped_core_files=$(/usr/bin/find -H .. bazel-testlogs -name "*.core.gz" 2>/dev/null)
for gzipped_core_file in $gzipped_core_files; do
    base_name=$(echo $gzipped_core_file | sed "s/.*\///" | sed "s/\.gz$//")
    # Decompress file if it does not already exist
    if [ ! -f $base_name ]; then
        gunzip -c $gzipped_core_file >$base_name
    fi
done
