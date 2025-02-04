/**
 * Test that verifies the javascript integration can handle large string exception messages.
 */
const len = 65 * 1024 * 1024;
const str = 'b'.repeat(len);

// We expect to successfully throw and catch this large exception message.
// We do not want the mongo shell to terminate.
assert.throws(function() {
    throw str;
});
