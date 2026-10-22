#!/bin/sh
# Give the action user a private probe directory and read-only input manifest.
probe=$1
owner=$2
umask 077
/bin/chmod 700 "$probe" &&
    /bin/chmod 600 "$probe/manifest" &&
    /bin/chown -- "$owner" "$probe/manifest" &&
    : >"$probe/ready" &&
    /bin/chown -- "$owner" "$probe"
