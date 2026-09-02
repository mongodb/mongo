if [ "Windows_NT" = "$OS" ]; then
    # Preserve the logical path behavior used by Windows+Cygwin.
    calculated_workdir=$(cd "$evergreen_dir/../.." && echo "$PWD")
    pwd_cygpath="$PWD"
    calculated_workdir=$(cygpath -w "$calculated_workdir")
    pwd_cygpath=$(cygpath -w "$pwd_cygpath")
else
    calculated_workdir=$(cd -P "$evergreen_dir/../.." && pwd -P)
    pwd_cygpath=$(pwd -P)
fi
if [ -z "$workdir" ]; then
    workdir="$calculated_workdir"

# skip this test on Windows. The directories will never match due to the many
# different path types present on Windows+Cygwin
elif [ "Windows_NT" != "$OS" ]; then
    # On macOS, Evergreen can provide the workdir through a symlink (for example,
    # /data/mci) while the checkout resolves to its mounted path. Compare
    # canonical paths so equivalent paths do not look like a moved checkout.
    if ! workdir_for_comparison=$(cd -P "$workdir" && pwd -P); then
        # Preserve the explicit mismatch diagnostic for a stale workdir.
        workdir_for_comparison=""
    fi

    if [ "$workdir_for_comparison" != "$calculated_workdir" ]; then
        # if you move the checkout directory (ex: simple project config project),
        # then this assertion will fail in the future. You need to update
        # calculated_workdir, and all the relative directories in this file.
        echo "\$workdir was specified, but didn't match \$calculated_workdir. Did the directory structure change? Update prelude.sh"
        echo "\$workdir: $workdir"
        echo "\$calculated_workdir: $calculated_workdir"
        exit 1
    fi
fi
if [ "$pwd_cygpath" != "$calculated_workdir" ]; then
    echo "ERROR: Your script changed directory before loading prelude.sh. Don't do that"
    echo "\$PWD: $PWD"
    echo "\$calculated_workdir: $calculated_workdir"
    exit 1
fi
