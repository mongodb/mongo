process_kill_list="(^cl\.exe$|java|lein|lldb|mongo|python|_test$|_test\.exe$)"
# Exclude Evergreen agent processes and other system daemons
process_exclude_list="(main|tuned|evergreen)"

if [ "Windows_NT" = "$OS" ]; then
  # Get the list of Windows tasks (tasklist list format):
  # - Transpose the Image Name and PID
  # - The first column has the process ID
  # - The second column (and beyond) has task name
  # - Grep for the task names of interest while ignoring any names that are in the exclude list

  processes=$(tasklist /fo:csv | awk -F'","' '{x=$1; gsub("\"","",x); print $2, x}' | grep -iE "$process_kill_list" | grep -ivE "$process_exclude_list")

  # Kill the Windows process by process ID with force (/f)
  kill_process() {
    pid=$(echo $1 | cut -f1 -d ' ')
    echo "Killing process $1"
    taskkill /pid "$pid" /f
  }
else
  # Get the list of Unix tasks (pgrep full & long):
  # - Grep for the task names of interest while ignoring any names that are in the exclude list
  # - The first column has the process ID
  # - The second column (and beyond) has task name

  # There are 2 "styles" of pgrep, figure out which one works.
  # Due to https://bugs.launchpad.net/ubuntu/+source/procps/+bug/1501916
  # we cannot rely on the return status ($?) to detect if the option is supported.
  pgrep -f --list-full ".*" 2>&1 | grep -qE "(illegal|invalid|unrecognized) option"
  if [ $? -ne 0 ]; then
    pgrep_list=$(pgrep -f --list-full "$process_kill_list")
  else
    pgrep_list=$(pgrep -f -l "$process_kill_list")
  fi

  # Since a process name might have a CR or LF in it, we need to delete any lines from
  # pgrep which do not start with space(s) and 1 digit and trim any leading spaces.
  processes=$(echo "$pgrep_list" | grep -ivE "$process_exclude_list" | sed -e '/^ *[0-9]/!d; s/^ *//; s/[[:cntrl:]]//g;')

  # Kill the Unix process ID with signal KILL (9)
  kill_process() {
    pid=$(echo $1 | cut -f1 -d ' ')
    echo "Killing process $1"
    kill -9 $pid
  }
fi
# Since a full process name can have spaces, the IFS (internal field separator)
# should not include a space, just a LF & CR
IFS=$(printf "\n\r")
for process in $processes; do
  kill_process "$process"
done

exit 0
