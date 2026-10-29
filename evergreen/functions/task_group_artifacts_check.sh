# Checks that build outputs an earlier task in this task group produced are present
# in this task's working directory. No prelude.sh, like the other check scripts
# here; REQUIRED_ARTIFACTS comes from the command's env.

set -o errexit

cd src

# Unquoted to split on whitespace. Paths stay relative to src: the failure being
# detected is running in the wrong working directory.
missing=()
for path in ${REQUIRED_ARTIFACTS}; do
    if [[ ! -e ${path} ]]; then
        missing+=("${path}")
    fi
done

if [[ ${#missing[@]} -eq 0 ]]; then
    exit 0
fi

echo "ERROR: build outputs from an earlier task in this task group are missing."
echo "Working directory: $(pwd)"
echo "Missing:"
for path in "${missing[@]}"; do
    echo "  - ${path}"
done
cat <<'EOF'

This task reads files that an earlier task in its single-host task group built. It
does not fetch or rebuild them, so it only works when it shares that task's working
directory.

Evergreen does not guarantee that. Later tasks in a group can end up on a different
host, and even on the same host can be given a newly allocated /data/mci/<hash>
directory, which will not contain what the producer built.

To confirm that is what happened, compare the working directory above against the
one logged by the task that builds these paths. If they differ, this is an
infrastructure failure rather than a problem with the code under test. If they
match, the producer did not build these paths and the cause is elsewhere.
EOF
exit 1
