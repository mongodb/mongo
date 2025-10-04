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
