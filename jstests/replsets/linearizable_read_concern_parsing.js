/*
 * This tests that commands passed with 'readConcern: linearizable' are parsed correctly. It
 * first expects a success on the primary node. Then it expects a failure when a
 * linearizable read concern is sent to the secondary node. Finally, it expects a
 * failure when the afterOpTime field is also provided.
 *
 */
load("jstests/replsets/rslib.js");
(function() {

    var replTest = new ReplSetTest({
        name: 'linearizable_read_concern_parsing',
        nodes: 3,
        nodeOptions: {enableMajorityReadConcern: ''}
    });

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return true;
    }
    replTest.initiate();

    var primary = replTest.getPrimary();
    primary.getDB("test").foo.insert({number: 2});

    var goodCmd = assert.commandWorked(
        primary.getDB("test").runCommand({'find': 'foo', readConcern: {level: "linearizable"}}));

    var secondary = replTest.getSecondary();
    var badCmd = assert.commandFailed(secondary.getDB("test").runCommand({
        'find': 'foo',
        readConcern: {level: "linearizable"},
    }));

    assert.eq(badCmd.errmsg, "cannot satisfy linearizable read concern on non-primary node");
    assert.eq(badCmd.code, ErrorCodes.NotMaster);

    var opTimeCmd = assert.commandFailed(primary.getDB("test").runCommand({
        'find': 'foo',
        readConcern: {level: "linearizable", 'afterOpTime': {ts: Timestamp(1, 2), t: 1}}
    }));
    assert.eq(opTimeCmd.errmsg, "afterOpTime not compatible with read concern level linearizable");
    assert.eq(opTimeCmd.code, ErrorCodes.FailedToParse);

}());