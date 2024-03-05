# Scripting Profiler

## Prerequisite

This is one time setup for a host.

```bash
# This cmd is for ubuntu20, different os may need different cmd. AWS (evergreen host) may require addition pkg.
sudo apt install linux-tools-generic
# Make sure `perf` command runs well, and /usr/bin/perf exists.
```

Run following commands to allow `perf` runs without root permission:

```bash
sudo sysctl -w kernel.perf_event_paranoid=1
sudo sysctl -w kernel.kptr_restrict=0
# This makes the change persistant across reboots
sudo echo "kernel.perf_event_paranoid=1\nkernel.kptr_restrict=0" > /etc/sysctl.d/perf.conf
```

## How to use

### Start the profiler

Syntax

```
db.adminCommand(
    {
        sysprofile: 1,
        filename: <string>,
        mode: <string>
    }
)
```

Parameters

-   filename: The output file of linux-perf without suffix
    -   type: `string`
    -   default value: `perf`
-   mode: The profiling mode
    -   type: `string`
    -   possible values: `record` or `counters`
    -   default value: `record`

Response

```
{ pid: <integrer>, ok: 1 }
```

### Stop the profiler

```
db.adminCommand(
    {
        sysprofile: 1,
        pid: <integer>
    }
)
```

Parameters:

-   pid: This is the perf process pid, it is returned when profiler is started.
    -   type: `integer`
    -   required: `true`

## Example

```bash
# Start the profiler.
let pid = db.adminCommand({sysprofile: 1, filename: "/home/ubuntu/tmp/perf_sbe"}).pid;
# Run some queries.
for (let i = 0; i < 1000; ++i) {
   db.search.find({}).toArray();
}
# Stop the profiler, and generate profiling data.
db.adminCommand({sysprofile: 1, pid: pid})

# Change some settings (e.g. using classic engine).
db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"})
# Start profiler again with a different file name.
pid = db.adminCommand({sysprofile: 1, filename: "/home/ubuntu/tmp/perf_classic"}).pid;
# Run some queries.
for (let i = 0; i < 1000; ++i) {
   db.search.find({}).toArray();
}
# Stop the profiler.
db.adminCommand({sysprofile: 1, pid: pid})

## Using Shell
perf report -i /home/ubuntu/tmp/perf_sbe.data
perf report -i /home/ubuntu/tmp/perf_classic.data
# Or generate flame graph
```
