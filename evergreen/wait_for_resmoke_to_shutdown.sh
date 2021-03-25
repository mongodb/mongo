proc="resmoke.py"
if [ "Windows_NT" = "$OS" ]; then
  check_resmoke() {
    resmoke_info=$(wmic process | grep resmoke.py)
  }
  while [ 1 ]; do
    check_resmoke
    if ! [[ "$resmoke_info" =~ .*"$proc".* ]]; then
      break
    fi
    sleep 5
  done
else
  get_pids() { proc_pids=$(pgrep -f $1); }
  while [ 1 ]; do
    get_pids $proc
    if [ -z "$proc_pids" ]; then
      break
    fi
    sleep 5
  done
fi
