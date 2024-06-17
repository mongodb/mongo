#!/bin/bash

# Check for illegal external symbols.
#
. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_dist

case `uname` in
Darwin)
    NM='nm -gUo $f | grep -E " T | D " | sed "s/ _/ /"'
    ;;
*)
    # We require GNU nm, which may not be installed.
    type nm > /dev/null 2>&1 &&
        (nm --version | grep 'GNU nm') > /dev/null 2>&1 || {
        echo "$0 skipped: GNU nm not found"
        exit 0
    }
    NM='nm --extern-only --defined-only --print-file-name $f | grep -E -v "__bss_start|_edata|_end|_fini|_init"'
    ;;
esac

check()
{
    (sed -e '/^#/d' s_export.list &&
    eval $NM |
    sed 's/.* //' |
    # Functions beginning with __ut are intentionally exposed to support unit testing when
    # Wiredtiger is compiled with HAVE_UNITTEST=1.
    grep -E -v '^__ut' |
    # __wrap_f?stat functions suppress MSan false positives. They're only present when compiled with MSan.
    grep -E -v '^__wrap_f?stat' |
    # MSan injected symbol present when origin tracking is enabled.
    grep -E -v '^__msan_track_origins' |
    grep -E -vi '^__wt') |
    sort |
    uniq -u |
    grep -E -v \
        'lz4_extension_init|snappy_extension_init|zlib_extension_init|zstd_extension_init' > $t

    test -s $t && {
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo 'unexpected external symbols in the WiredTiger library '"$f"
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        cat $t
        exit 1
    }
}

# This check would normally be done after the library is built, but this way
# we don't forget about a symbol during development. We usually build in the
# top-level directories, check the previously built library,
# if it exists. And, allow this script to be run from the top-level directory
# as well as locally. (??? this last wasn't true before my changes and still
# isn't)
#
# Check all library images that appear; typically they will be one of
#    ../build/libwiredtiger.$ext                (cmake build)
#    ../cmake_build/libwiredtiger.$ext          (cmake build)

found=0
for f in $(find .. -name "libwiredtiger.*" -print); do
    case "$f" in
        *.a|*.so|*.dylib)
        check "$f"
        found=1
            ;;
        *)
        ;;
    esac
done

if [ $found = 0 ]; then
    echo "$0 skipped: libwiredtiger.[a|so|dylib] not found"
fi
exit 0
