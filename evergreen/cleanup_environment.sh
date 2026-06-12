set -o verbose

# Determine whether sudo is available without sourcing prelude.sh (which requires the venv and
# would fail in tasks where the venv was never initialized).
sudo=
sudo -n date >/dev/null 2>&1 && sudo=sudo

rm -rf \
    /data/db/* \
    mongo-diskstats* \
    mongo-*.tgz \
    mongo-*.zst \
    ~/.aws \
    ~/.boto \
    venv \
    /data/install \
    /data/multiversion

# If root-owned files remain in /data/db (e.g. left by SLS Docker containers), retry with sudo.
if find /data/db -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null | grep -q .; then
    $sudo rm -rf /data/db/*
fi

exit 0
