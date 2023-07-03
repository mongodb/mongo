. "$COMMON/platform.sh"

MTIME="stat -c %Y"
case "$UNAME" in
    Darwin | FreeBSD | OpenBSD | NetBSD) MTIME="stat -f %m" ;;
esac

assertSameMTime() {
    MT1=$($MTIME "$1")
    MT2=$($MTIME "$2")
    echo MTIME $MT1 $MT2
    [ "$MT1" = "$MT2" ] || die "mtime on $1 doesn't match mtime on $2 ($MT1 != $MT2)"
}
