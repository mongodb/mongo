set -o verbose

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

exit 0
