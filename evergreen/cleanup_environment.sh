set -o verbose

rm -rf \
  /data/db/* \
  mongo-diskstats* \
  mongo-*.tgz \
  ~/.aws \
  ~/.boto \
  venv \
  /data/install \
  /data/multiversion

exit 0
