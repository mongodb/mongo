DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

proc_list="(java|lein|mongo|python|_test$|_test\.exe$)"
if [ "Windows_NT" = "$OS" ]; then
  get_pids() {
    proc_pids=$(tasklist /fo:csv \
      | awk -F'","' '{x=$1; gsub("\"","",x); print $2, x}' \
      | grep -iE $1 \
      | cut -f1 -d ' ')
  }
  get_process_info() {
    proc_name=""
    proc_info=$(wmic process where "ProcessId=\"$1\"" get "Name,ProcessId,ThreadCount" /format:csv 2> /dev/null | grep $1)
    if [ ! -z $proc_info ]; then
      proc_name=$(echo $proc_info | cut -f2 -d ',')
      proc_threads=$(echo $proc_info | cut -f4 -d ',')
    fi
  }
else
  get_pids() { proc_pids=$(pgrep $1); }
  get_process_info() {
    proc_name=$(ps -p $1 -o comm=)
    # /proc is available on Linux platforms
    if [ -f /proc/$1/status ]; then
      set_sudo
      proc_threads=$($sudo grep Threads /proc/$1/status | sed "s/\s//g" | cut -f2 -d ":")
    else
      proc_threads=$(ps -AM $1 | grep -vc PID)
    fi
  }
fi
while [ 1 ]; do
  get_pids $proc_list
  if [ ! -z "$proc_pids" ]; then
    printf "Running process/thread counter\n"
    printf "PROCESS\tPID\tTHREADS\n"
  fi
  for pid in $proc_pids; do
    get_process_info $pid
    if [ ! -z "$proc_name" ]; then
      printf "$proc_name\t$pid\t$proc_threads\n"
    fi
  done
  sleep 60
done
