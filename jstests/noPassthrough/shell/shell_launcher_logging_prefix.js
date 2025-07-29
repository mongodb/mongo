// Tests that _startMongoProgram adds the PID to the program output.
// By default, the program output will contain the prefix "sh<PID>".
// If a logging prefix is provided, the program output will include "<prefix>:<PID>".

clearRawMongoProgramOutput();
const pidWithDefaultLoggingPrefix =
    _startMongoProgram({args: ["echo", "hello with default logging prefix"]});
jsTest.log.info(
    `Started program with default logging prefix, PID: ${tojson(pidWithDefaultLoggingPrefix)}`);
assert(waitProgram(pidWithDefaultLoggingPrefix) == 0,
       `Failed to run program with default logging prefix`);

// The pid is a NumberLong so we have to extract the numerical value to avoid including the string
// "NumberLong" in the prefix.
assert(rawMongoProgramOutput(".*").match(
           `sh${pidWithDefaultLoggingPrefix.valueOf()}| .*hello with default prefix`),
       "Expected output to include default prefix and PID");

clearRawMongoProgramOutput();
const pidWithCustomLoggingPrefix = _startMongoProgram(
    {args: ["echo", "hello with custom logging prefix"], loggingPrefix: "myapp"});
jsTest.log.info(
    `Started program with custom logging prefix, PID: ${tojson(pidWithCustomLoggingPrefix)}`);
assert(waitProgram(pidWithCustomLoggingPrefix) == 0,
       `Failed to run program with custom logging prefix`);
assert(rawMongoProgramOutput(".*").match(
           `myapp:${pidWithCustomLoggingPrefix.valueOf()}| .*hello with custom logging prefix`),
       "Expected output to include myapp and PID");
