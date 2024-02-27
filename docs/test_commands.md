# Test Commands

All test commands are denoted with the `.testOnly()` modifier to the `MONGO_REGISTER_COMMAND` invocation.
For example:

```c++
MONGO_REGISTER_COMMAND(EchoCommand).testOnly();
```

## How to enable

To be able to run these commands, the server must be started with the `enableTestCommands=1`
server parameter (e.g. `--setParameter enableTestCommands=1`). Resmoke.py often sets this server
parameter for testing.

## Examples

Some often-used commands that are test-only:

-   [configureFailPoint][fail_point_cmd]
-   [emptyCapped][empty_capped_cmd]
-   [replSetTest][repl_set_test_cmd]
-   [sleep][sleep_cmd]

As a very rough estimate, about 10% of all server commands are test-only. These additional commands
will appear in `db.runCommand({listCommands: 1})` when the server has test commands enabled.

## Test Command Infrastructure

A few pointers to relevant code that sets this up:

-   [test_commands_enabled.h][test_commands_enabled]

-   [MONGO_REGISTER_COMMAND][register_command]

[empty_capped_cmd]: ../src/mongo/db/commands/test_commands.cpp
[fail_point_cmd]: ../src/mongo/db/commands/fail_point_cmd.cpp
[register_command]: ../src/mongo/db/commands.h
[repl_set_test_cmd]: ../src/mongo/db/repl/repl_set_commands.cpp
[sleep_cmd]: ../src/mongo/db/commands/sleep_command.cpp
[test_commands_enabled]: ../src/mongo/db/commands/test_commands_enabled.h
