# Powercycle README

Power cycling is the process of turning hardware off and then turning it on again.
Powercycle test is designed to work across two machines, one machine is a "server"
that controls and monitors the workflow and a "client" that runs Mongo server and
is remotely crashed by "server" regularly.

In evergreen the localhost that runs the task acts as a "server" and the remote
host which is created by `host.create` evergreen command acts as a "client".

Powercycle test is the part of resmoke. Python 3.10+ with python venv is required to
run the resmoke (python3 from [mongodbtoolchain](http://mongodbtoolchain.build.10gen.cc/)
is highly recommended). Python venv can be set up by running in the root mongo repo
directory:

```
python3 -m venv python3-venv
source python3-venv/bin/activate
pip install -r buildscripts/requirements.txt
```

If python venv is already set up activate it before running the resmoke:

```
source python3-venv/bin/activate
```

There are several commands that can be run by calling resmoke powercycle subcommand:

```
python buildscripts/resmoke.py powercycle --help
```

The main entry point of resmoke powercycle subcommand is located in this file:

```
buildscripts/resmokelib/powercycle/__init__.py
```

## Poweryclce main steps

-   [Set up EC2 instance](#set-up-ec2-instance)
-   [Run powercycle test](#run-powercycle-test)
    -   [Resmoke powercycle run arguments](#resmoke-powercycle-run-arguments)
    -   [Powercycle test implementation](#powercycle-test-implementation)
-   [Save diagnostics](#save-diagnostics)
-   [Remote hang analyzer (optional)](#remote-hang-analyzer-optional)

### Set up EC2 instance

1. `Evergreen host.create command` - in Evergreen the remote host is created with
   the same distro as the localhost runs and some initial connections are made to ensure
   it's up before further steps
2. `Resmoke powercycle setup-host command` - prepares remote host via ssh to run
   the powercycle test:

```
python buildscripts/resmoke.py powercycle setup-host
```

Powercycle setup-host operations are located in
`buildscripts/resmokelib/powercycle/setup/__init__.py`.
`expansions.yml` file is used to load the configuration to run operations which is
created by `expansions.write` command in Evergreen.

It runs several operations via ssh:

-   create directory on the remote host
-   copy `buildscripts` and `mongoDB executables` from localhost to the remote host
-   set up python venv on the remote host
-   set up curator to collect system & process stats on the remote host
-   install [NotMyFault](https://docs.microsoft.com/en-us/sysinternals/downloads/notmyfault)
    to crash Windows (only on Windows)

Remote operation via ssh implementation is located in
`buildscripts/resmokelib/powercycle/lib/remote_operations.py`.
The following operations are supported:

-   `copy_to` - copy files from the localhost to the remote host
-   `copy_from` - copy files from the remote host to the localhost
-   `shell` - runs shell command on the remote host

### Run powercycle test

`Resmoke powercycle run command` - runs the powercycle test on the localhost
which runs remote operations on the remote host via ssh and local validation
checks:

```
python buildscripts/resmoke.py powercycle run \
    --sshUserHost=${user_name}@${host_ip} \
    --sshConnection=\"-i ${ssh_public_key_file}\" \
    --taskName=${task_name}
```

###### Resmoke powercycle run arguments

The arguments for resmoke powercycle run command are defined in `add_subcommand()`
function in `buildscripts/resmokelib/powercycle/__init__.py`. When powercycle test
runs remote operations on the remote host it calls the copied version of this script
on the remote host. Thus, some resmoke powercycle run command arguments are needed
for the remote call and shouldn't be used when calling the script on the localhost.

`--taskName` argument is used to get powercycle task configurations that are stored
in `buildscripts/resmokeconfig/powercycle/powercycle_tasks.yml`

There is a known issue with `--setParameter` mongod options incorrectly processed
from `mongod_options` that is described in [SERVER-47621](https://jira.mongodb.org/browse/SERVER-47621)

###### Powercycle test implementation

The powercycle test main implementation is located in `main()` function in
`buildscripts/resmokelib/powercycle/powercycle.py`.

The value of `--remoteOperation` argument is used to distinguish if we are running the script
on the localhost or on the remote host.
`remote_handler()` function performs the following remote operations:

-   `noop` - do nothing
-   `crash_server` - internally crash the server
-   `kill_mongod` - kill mongod process
-   `install_mongod` - install mongod
-   `start_mongod` - start mongod process
-   `stop_mongod` - stop mongod process
-   `shutdown_mongod` - run shutdown command using mongo client
-   `rsync_data` - backups mongod data
-   `seed_docs` - seed a collection with random document values
-   `set_fcv` - run set FCV command using mongo client
-   `check_disk` - run `chkdsk` command on Windows

When running on localhost the powercycle test loops do the following steps:

-   Rsync the database post-crash (starting from the 2nd loop), pre-recovery on the remote host
    -   makes a backup before recovery
-   Start mongod on the secret port on the remote host and wait for it to recover
    -   also sets FCV and seeds documents on the 1st loop
-   Validate canary from the localhost (starting from the 2nd loop)
    -   uses mongo client to connect to the remote mongod
-   Validate collections from the localhost
    -   calls resmoke to perform the validation on the remote mongod
-   Shutdown mongod on the remote host
-   Rsync the database post-recovery on the remote host
    -   makes a backup after recovery
-   Start mongod on the standard port on the remote host
-   Start CRUD and FSM clients on the localhost
    -   calls resmoke to run CRUD and FSM clients
-   Generate canary document from the localhost
    -   uses mongo client to connect to the remote mongod
-   Crash the remote server or kill mongod on the remote host
    -   most of the powercycle tasks do crashes
-   Run check disk on the remote host (on Windows)
-   Exit loop if one of these occurs:
    -   loop number exceeded
    -   any step fails

`exit_handler()` function writes a report and does cleanups any time after the test run exits.

### Save diagnostics

`Resmoke powercycle save-diagnostics command` - copies powercycle diagnostics
files from the remote host to the localhost (mainly used by Evergreen):

```
python buildscripts/resmoke.py powercycle save-diagnostics
```

Powercycle save-diagnostics operations are located in
`buildscripts/resmokelib/powercycle/save_diagnostics/__init__.py`.
`expansions.yml` file is used to load the configuration to run operations which is
created by `expansions.write` command in Evergreen.

It runs several operations via ssh:

-   `gatherRemoteEventLogs`
    -   runs on Windows
-   `tarEC2Artifacts`
    -   on success archives `mongod.log`
    -   on failure additionally archives data files and all before-recovery and after-recovery backups
    -   on failure on Windows additionally archives event logs
-   `copyEC2Artifacts`
    -   from the remote host to the localhost
-   `copyEC2MonitorFiles`
    -   from the remote host to the localhost
-   `gatherRemoteMongoCoredumps`
    -   copies all mongo core dumps to a single directory
-   `copyRemoteMongoCoredumps`
    -   from the remote host to the localhost

### Remote hang analyzer (optional)

`Resmoke powercycle remote-hang-analyzer command` - runs hang analyzer on the
remote host (mainly used by Evergreen):

```
$python buildscripts/resmoke.py powercycle remote-hang-analyzer
```

Powercycle remote-hang-analyzer command calls resmoke hang analyzer on the
remote host and is located in
`buildscripts/resmokelib/powercycle/remote_hang_analyzer/__init__.py`
`expansions.yml` file is used to load the configuration to run this command which is
created by `expansions.write` command in Evergreen.
