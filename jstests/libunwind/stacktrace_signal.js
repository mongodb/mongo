/**
 * Hit mongod with a SIGUSR2 and observe multithread stack trace.
 * Should only work on Linux, and when mongod is built with libunwind.
 *
 * @tags: [
 *   requires_libunwind
 * ]
 */
clearRawMongoProgramOutput();
const conn = MongoRunner.runMongod();
// convert the float to a string to make sure it's correctly represented.
runNonMongoProgram("/bin/kill", "-s", "SIGUSR2", conn.pid.valueOf().toString());
MongoRunner.stopMongod(conn);
const output = rawMongoProgramOutput("(processInfo|backtrace)");
assert(output.search(/"processInfo":/) >= 0, output);
// Will be several of these
assert(output.search(/"backtrace":/) >= 0, output);
