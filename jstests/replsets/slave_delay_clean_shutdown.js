// SERVER-21118 don't hang at shutdown or apply ops too soon with slaveDelay.
//
// @tags: [requires_persistence]
load('jstests/replsets/rslib.js');
(function() {
    "use strict";

    var ns = "test.coll";

    var rst = new ReplSetTest({
        nodes: 2,
    });

    var conf = rst.getReplSetConfig();
    conf.members[1].votes = 0;
    conf.members[1].priority = 0;
    conf.members[1].hidden = true;
    conf.members[1].slaveDelay = 0;  // Set later.

    rst.startSet();
    rst.initiate(conf);

    var master = rst.getPrimary();  // Waits for PRIMARY state.

    // Push some ops through before setting slave delay.
    assert.writeOK(master.getCollection(ns).insert([{}, {}, {}], {writeConcern: {w: 2}}));

    // Set slaveDelay and wait for secondary to receive the change.
    conf = rst.getReplSetConfigFromNode();
    conf.version++;
    conf.members[1].slaveDelay = 24 * 60 * 60;
    reconfig(rst, conf);
    assert.soon(() => rst.getReplSetConfigFromNode(1).members[1].slaveDelay > 0,
                () => rst.getReplSetConfigFromNode(1));

    sleep(2000);  // The secondary apply loop only checks for slaveDelay changes once per second.
    var secondary = rst.getSecondary();
    const lastOp = getLatestOp(secondary);

    assert.writeOK(master.getCollection(ns).insert([{}, {}, {}]));
    assert.soon(() => secondary.adminCommand('serverStatus').metrics.repl.buffer.count > 0,
                () => secondary.adminCommand('serverStatus').metrics.repl);
    assert.neq(getLatestOp(master), lastOp);
    assert.eq(getLatestOp(secondary), lastOp);

    sleep(2000);  // Prevent the test from passing by chance.
    assert.eq(getLatestOp(secondary), lastOp);

    // Make sure shutdown won't take a long time due to I/O.
    secondary.adminCommand('fsync');

    // Shutting down shouldn't take long.
    assert.lt(Date.timeFunc(() => rst.stop(1)), 60 * 1000);

    secondary = rst.restart(1);
    assert.eq(getLatestOp(secondary), lastOp);
    sleep(2000);  // Prevent the test from passing by chance.
    assert.eq(getLatestOp(secondary), lastOp);

    rst.stopSet();
})();
