DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/jepsen-mongodb

set -o verbose

# Set the TMPDIR environment variable to be a directory in the task's working
# directory so that temporary files created by processes spawned by jepsen get
# cleaned up after the task completes. This also ensures the spawned processes
# aren't impacted by limited space in the mount point for the /tmp directory.
# We also need to set the _JAVA_OPTIONS environment variable so that lein will
# recognize this as the default temp directory.
export TMPDIR="${workdir}/tmp"
mkdir -p $TMPDIR
export _JAVA_OPTIONS=-Djava.io.tmpdir=$TMPDIR

start_time=$(date +%s)
lein run test --test ${jepsen_test_name} \
  --mongodb-dir ../ \
  --working-dir ${workdir}/src/jepsen-workdir \
  --clock-skew faketime \
  --libfaketime-path ${workdir}/src/libfaketime/build/libfaketime.so.1 \
  --mongod-conf mongod_verbose.conf \
  --virtualization none \
  --nodes-file ../nodes.txt \
  ${jepsen_key_time_limit} \
  ${jepsen_protocol_version} \
  ${jepsen_read_concern} \
  ${jepsen_read_with_find_and_modify} \
  ${jepsen_storage_engine} \
  ${jepsen_time_limit} \
  ${jepsen_write_concern} \
  2>&1 \
  | tee jepsen_${task_name}_${execution}.log
end_time=$(date +%s)
elapsed_secs=$((end_time - start_time))
. ../evergreen/jepsen_report.sh
