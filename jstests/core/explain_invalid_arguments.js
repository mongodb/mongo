/**
 * Tests for validating arguments of the explain command:
 *  -- SERVER-27976 Do not accept unknown argument for the explain command.
 */
(function() {
"use strict";

var collName = "jstests_explain_invalid_arguments";

var explain = db.runCommand({explain: {find: collName}, verbosity: "executionStats"});
assert.commandWorked(explain);

explain = db.runCommand({explain: {find: collName}, verosity: "executionStats"});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'verosity' argument.", explain.errmsg);

explain = db.runCommand({explain: {find: collName}, verbosity: "executionStats", foo: 1});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'foo' argument.", explain.errmsg);

explain = db.runCommand({explain: {find: collName}, foo: 1, bar: 1});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'foo' argument.", explain.errmsg);

explain = db.runCommand({explain: {find: collName}, foo: 1, verbosity: "executionStats", bar: 1});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'foo' argument.", explain.errmsg);

explain = db.runCommand({explain: {find: collName}, explain: {}, bar: 1});
assert.commandFailedWithCode(explain, ErrorCodes.InvalidOptions);
assert.eq("explain does not support 'bar' argument.", explain.errmsg);
})();
