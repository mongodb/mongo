function tryParallelShell() {
    var nevercalled = startParallelShell("");
    // The shell running this function will generate a non-zero exit code
    // because nevercalled isn't called.
}

var ret = startParallelShell(tryParallelShell);

assert.throws(ret);
// Since ret is called by assert.throws, the shell running this file will
// exit cleanly
