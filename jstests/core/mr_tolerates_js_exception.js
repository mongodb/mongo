// @tags: [does_not_support_stepdowns]

/**
 *  Test that the mapReduce command fails gracefully when user-provided JavaScript code throws.
 */
(function() {
    "use strict";

    let coll = db.mr_tolerates_js_exception;
    coll.drop();
    for (let i = 0; i < 100; i++) {
        assert.writeOK(coll.insert({_id: i, a: 1}));
    }

    // Test that the command fails with a JS interpreter failure error when the reduce function
    // throws.
    let cmdOutput = db.runCommand({
        mapReduce: coll.getName(),
        map: function() {
            emit(this.a, 1);
        },
        reduce: function(key, value) {
            throw 42;
        },
        out: {inline: 1}
    });
    assert.commandFailedWithCode(cmdOutput, ErrorCodes.JSInterpreterFailure, tojson(cmdOutput));

    // Test that the command fails with a JS interpreter failure error when the map function
    // throws.
    cmdOutput = db.runCommand({
        mapReduce: coll.getName(),
        map: function() {
            throw 42;
        },
        reduce: function(key, value) {
            return Array.sum(value);
        },
        out: {inline: 1}
    });
    assert.commandFailedWithCode(cmdOutput, ErrorCodes.JSInterpreterFailure, tojson(cmdOutput));
}());
