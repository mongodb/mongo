// Tests the replSetStepUp command.

load("jstests/replsets/rslib.js");

(function() {

    "use strict";
    var name = "stepup";
    var rst = new ReplSetTest({name: name, nodes: 2});

    rst.startSet();
    // Initiate the replset in protocol version 0.
    var conf = rst.getReplSetConfig();
    conf.protocolVersion = 0;
    rst.initiate(conf);
    rst.awaitReplication();

    var primary = rst.getPrimary();
    var secondary = rst.getSecondary();
    var res = secondary.adminCommand({replSetStepUp: 1});
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);

    // Upgrade protocol version
    conf = rst.getReplSetConfigFromNode();
    conf.protocolVersion = 1;
    conf.version++;
    reconfig(rst, conf);
    // Wait for the upgrade to finish.
    assert.writeOK(primary.getDB("test").bar.insert({x: 1}, {writeConcern: {w: 2}}));

    // Step up the primary. Return OK because it's already the primary.
    res = primary.adminCommand({replSetStepUp: 1});
    assert.commandWorked(res);
    assert.eq(primary, rst.getPrimary());

    // Step up the secondary, but it's not eligible to be primary.
    // Enable fail point on secondary.
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));

    assert.writeOK(primary.getDB("test").bar.insert({x: 2}, {writeConcern: {w: 1}}));
    res = secondary.adminCommand({replSetStepUp: 1});
    assert.commandFailedWithCode(res, ErrorCodes.CommandFailed);
    assert.commandWorked(
        secondary.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));

    // Wait for the secondary to catch up by replicating a doc to both nodes.
    assert.writeOK(primary.getDB("test").bar.insert({x: 3}, {writeConcern: {w: "majority"}}));
    // Step up the secondary and succeed.
    res = secondary.adminCommand({replSetStepUp: 1});
    assert.commandWorked(res);
    assert.eq(secondary, rst.getPrimary());

})();
