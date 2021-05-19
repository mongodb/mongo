/**
 * Test that the server errors when given an invalid regex.
 * @tags: [
 * ]
 */
(function() {
const coll = db.regex_error;
coll.drop();

// Run some invalid regexes.
assert.commandFailedWithCode(coll.runCommand("find", {filter: {a: {$regex: "[)"}}}), 51091);
assert.commandFailedWithCode(coll.runCommand("find", {filter: {a: {$regex: "ab\0c"}}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(
    coll.runCommand("find", {filter: {a: {$regex: "ab", $options: "\0i"}}}), 51108);
})();
