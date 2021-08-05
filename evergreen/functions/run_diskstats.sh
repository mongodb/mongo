set -o errexit
set -o verbose

# On Windows we can use typeperf.exe to dump performance counters.
if [ "Windows_NT" = "$OS" ]; then
  typeperf -qx PhysicalDisk | grep Disk | grep -v _Total > disk_counters.txt
  typeperf -cf disk_counters.txt -si 5 -o mongo-diskstats
# Linux: iostat -t option for timestamp.
elif iostat -tdmx > /dev/null 2>&1; then
  iostat -tdmx 5 > mongo-diskstats
# OSX: Simulate the iostat timestamp.
elif iostat -d > /dev/null 2>&1; then
  iostat -d -w 5 | while IFS= read -r line; do printf '%s %s\n' "$(date +'%m/%d/%Y %H:%M:%S')" "$line" >> mongo-diskstats; done
# Check if vmstat -t is available.
elif vmstat -td > /dev/null 2>&1; then
  vmstat -td 5 > mongo-diskstats
# Check if vmstat -T d is available.
elif vmstat -T d > /dev/null 2>&1; then
  vmstat -T d 5 > mongo-diskstats
else
  printf "Cannot collect mongo-diskstats on this platform\n"
fi
