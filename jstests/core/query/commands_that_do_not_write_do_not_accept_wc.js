/**
 * This file tests commands that do not support write concern. It passes both valid and invalid
 * writeConcern fields to commands and expects the commands to fail with a writeConcernNotSupported
 * error.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: mapReduce.
 *   not_allowed_with_signed_security_token,
 *   assumes_write_concern_unchanged,
 *   does_not_support_stepdowns,
 * ]
 */

let collName = "leaves";

let commands = [];

commands.push({find: collName, filter: {_id: 1}});

commands.push({distinct: collName, key: "_id"});

commands.push({count: collName, query: {type: "oak"}});

commands.push({
    mapReduce: collName,
    map: function () {
        this.tags.forEach(function (z) {
            emit(z, 1);
        });
    },
    reduce: function (key, values) {
        return {count: values.length};
    },
    out: {inline: 1},
});

function assertWriteConcernNotSupportedError(res) {
    assert.commandFailed(res);
    assert.eq(res.code, ErrorCodes.InvalidOptions);
    assert(!res.writeConcernError);
}

// Test a variety of valid and invalid writeConcerns to confirm that they still all get
// the correct error.
let writeConcerns = [{w: "invalid"}, {w: 1}];

function testUnsupportedWriteConcern(wc, cmd) {
    cmd.writeConcern = wc;
    jsTest.log("Testing " + tojson(cmd));

    let res = db.runCommand(cmd);
    assertWriteConcernNotSupportedError(res);
}

// Verify that each command gets a writeConcernNotSupported error.
commands.forEach(function (cmd) {
    writeConcerns.forEach(function (wc) {
        testUnsupportedWriteConcern(wc, cmd);
    });
});
