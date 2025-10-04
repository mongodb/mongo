# Evaluate this file with the source command from somewhere within
# your git repository workspace.

TOP__=$(git rev-parse --show-toplevel)
if ! [[ $? ]]; then
    echo "FATAL Not a git repo"
    return
fi

SO__=${TOP__}/TCMALLOC_LIB/libtcmalloc.so
if ! [[ -f $SO__ ]]; then
    echo "FATAL libtcmalloc.so not found"
    return
fi

eval "with_tcmalloc() { LD_PRELOAD=$SO__:$LD_PRELOAD \"\$@\"; }"

unset TOP__ SO__
