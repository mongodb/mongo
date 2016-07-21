// Note: This test cannot be run in parallel because all output from child processes of the same
// shell is multiplexed to the same buffer.
(function() {
    "use strict";

    // Note: the windows command line length limit is 8191 characters, so keep this string length
    // under that.
    const numLines = 300;
    const lineContents = "lots of super fun text\n".repeat(numLines).trim();

    var echoTest = function() {
        clearRawMongoProgramOutput();

        // This will produce `numLines` + 1 lines of output because echo isn't being called with
        // `-n`. This will block until the program exits.
        var exitCode = runProgram("echo", lineContents);
        var output = rawMongoProgramOutput();

        assert.eq(0, exitCode);

        assert.eq(numLines,
                  output.split('\n').length - 1,
                  "didn't wait for program's output buffer to finish being consumed");
    };

    // The motivating failure for the test was a race in runProgram. Empirically, 10 runs has always
    // been sufficient for this to fail. 16 gives the test some leeway.
    for (var i = 0; i < 16; i++) {
        echoTest();
    }

})();
