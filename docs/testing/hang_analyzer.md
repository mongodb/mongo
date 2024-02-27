# Hang Analyzer

The hang analyzer is a tool to collect cores and other information from processes
that are suspected to have hung. Any task which exceeds its timeout in Evergreen
will automatically be hang-analyzed, with information being written compressed
and uploaded to S3.

The hang analyzer can also be invoked locally at any time. For all non-Jepsen
tasks, the invocation is `buildscripts/resmoke.py hang-analyzer -o file -o stdout -m exact -p python`. You may need to substitute `python` with the name of the python binary
you are using, which may be one of `python`, `python3`, or on Windows: `Python`,
`Python3`.

For jepsen tasks, the invocation is `buildscripts/resmoke.py hang-analyzer -o file -o stdout -p dbtest,java,mongo,mongod,mongos,python,_test`.

## Interesting Processes

The hang analyzer detects and runs against processes which are considered
interesting.

Tasks whose name contains "jepsen": any process whose name exactly matches one
of `dbtest,java,mongo,mongod,mongos,python,_test`.

In all other scenarios, including local use of the hang-analyzer, an interesting
process is any of:

-   process that starts with `python` or `live-record`
-   one which has been spawned as a child process of resmoke.

The resmoke subcommand `hang-analyzer` will send SIGUSR1/use SetEvent to signal
resmoke to:

-   Print stack traces for all python threads
-   Collect core dumps and other information for any non-python child
    processes, see `Data Collection` below
-   Re-signal any python child processes to do the same

## Data Collection

Data collection occurs in the following sequence:

-   Pause all non-python processes
-   Grab debug symbols on non-Sanitizer builds
-   Signal python Processes
-   Dump cores of as many processes as possible, until the disk quota is exceeded.
    The default quota is 90% of total volume space.

-   Collect additional, non-core data. Ideally:
    -   Print C++ Stack traces
    -   Print MozJS Stack Traces
    -   Dump locks/mutexes info
    -   Dump Server Sessions
    -   Dump Recovery Units
    -   Dump Storage engine info
-   Dump java processes (Jepsen tests) with jstack
-   SIGABRT (Unix)/terminate (Windows) go processes

Note that the list of non-core data collected is only accurate on Linux. Other
platforms only perform a subset of these operations.

Additionally, note that the hang analyzer is subject to Evergreen post task
timeouts, and may not have enough time to collect all information before
being terminated by the Evergreen agent. When running locally there is no
timeout, and the hang analyzer may ironically hang indefinitely.

### Implementations

Platform-specific concerns for data collection are handled by dumper objects in
`buildscripts/resmokelib/hang_analyzer/dumper.py`.

-   Linux: See `GDBDumper`
-   MacOS: See `LLDBDumper`
-   Windows: See `WindowsDumper` and `JstackWindowsDumper`
-   Java (non-Windows): `JstackDumper`
