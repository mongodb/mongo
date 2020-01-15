/**
 * Hit mongod with a SIGUSR2 and observe multithread stack trace.
 * Should only work on Linux, and when mongod is built with libunwind.
 *
 * @tags: [
 *   requires_libunwind
 * ]
 */
(function() {
clearRawMongoProgramOutput();
const conn = MongoRunner.runMongod();
runMongoProgram('/bin/kill', '-s', 'SIGUSR2', conn.pid);
MongoRunner.stopMongod(conn);
const output = rawMongoProgramOutput();
assert(output.search(/"processInfo":/) >= 0, output);
// Will be several of these
assert(output.search(/"backtrace":/) >= 0, output);
})();
