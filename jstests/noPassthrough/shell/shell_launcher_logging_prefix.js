// Tests that _startMongoProgram adds the PID to the program output.
// By default, the program output will contain the prefix "sh<PID>".
// If a logging prefix is provided, the program output will include "sh_<prefix>:<PID>".

clearRawMongoProgramOutput();
const pidWithDefaultLoggingPrefix = _startMongoProgram({args: ["echo", "hello with default logging prefix"]});
jsTest.log.info(`Started program with default logging prefix, PID: ${tojson(pidWithDefaultLoggingPrefix)}`);
assert(waitProgram(pidWithDefaultLoggingPrefix) == 0, `Failed to run program with default logging prefix`);

// The pid is a NumberLong so we have to extract the numerical value to avoid including the string
// "NumberLong" in the prefix.
let pattern = `sh${pidWithDefaultLoggingPrefix.valueOf()}\\| .*hello with default logging prefix`;
let output = rawMongoProgramOutput(".*").split("\n");
let filteredOutput = output.filter((line) => line.match(pattern));
assert.eq(
    filteredOutput.length,
    1,
    `Expected output to include default prefix and PID (pattern: ${pattern}): ${tojson(output)}`,
);

clearRawMongoProgramOutput();
const pidWithCustomLoggingPrefix = _startMongoProgram({
    args: ["echo", "hello with custom logging prefix"],
    loggingPrefix: "myapp1",
});
jsTest.log.info(`Started program with custom logging prefix, PID: ${tojson(pidWithCustomLoggingPrefix)}`);
assert(waitProgram(pidWithCustomLoggingPrefix) == 0, `Failed to run program with custom logging prefix`);
pattern = `sh_myapp1:${pidWithCustomLoggingPrefix.valueOf()}\\| .*hello with custom logging prefix`;
output = rawMongoProgramOutput(".*").split("\n");
filteredOutput = output.filter((line) => line.match(pattern));
assert.eq(
    filteredOutput.length,
    1,
    `Expected output to include myapp1 and PID (pattern: ${pattern}): ${tojson(output)}`,
);
