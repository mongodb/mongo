. "$COMMON/platform.sh"

GET_PERMS="stat -c %a"
case "$UNAME" in
    Darwin | FreeBSD | OpenBSD | NetBSD) GET_PERMS="stat -f %Lp" ;;
esac

assertFilePermissions() {
    STAT1=$($GET_PERMS "$1")
    STAT2=$2
    [ "$STAT1" = "$STAT2" ] || die "permissions on $1 don't match expected ($STAT1 != $STAT2)"
}

assertSamePermissions() {
    STAT1=$($GET_PERMS "$1")
    STAT2=$($GET_PERMS "$2")
    [ "$STAT1" = "$STAT2" ] || die "permissions on $1 don't match those on $2 ($STAT1 != $STAT2)"
}
