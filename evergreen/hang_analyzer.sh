DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o verbose

# Set what processes to look for. For most tasks, we rely on resmoke to figure out its subprocesses
# and run the hang analyzer on those. For non-resmoke tasks, we enumerate the process list here.
if [[ ${task_name} == *"jepsen"* ]]; then
  hang_analyzer_option="-o file -o stdout -p dbtest,java,mongo,mongod,mongos,python,_test"
else
  hang_analyzer_option="-o file -o stdout -m exact -p python"
fi

activate_venv
echo "Calling the hang analyzer: PATH=\"/opt/mongodbtoolchain/gdb/bin:$PATH\" $python buildscripts/resmoke.py hang-analyzer $hang_analyzer_option"
PATH="/opt/mongodbtoolchain/gdb/bin:$PATH" $python buildscripts/resmoke.py hang-analyzer $hang_analyzer_option

# Call hang analyzer for tasks that are running remote mongo processes
if [ -n "${private_ip_address}" ]; then
  $python buildscripts/resmoke.py powercycle remote-hang-analyzer
fi
