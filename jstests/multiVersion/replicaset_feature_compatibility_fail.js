/**
 * This test verifies that a secondary node with a lower MongoDB version exits
 * with an EXIT_NEED_UPGRADE signal when connecting to a primary of a higher
 * version.
 */

(function() {

    // Primary should run latest, and secondary should run previous version.
    let replTest = new ReplSetTest({
        name: 'testSet',
        nodes: [
            {
              binVersion: "latest",
            },
            {
              binVersion: "last-stable",
            }
        ],
    });

    let nodes = replTest.startSet();
    let conf = replTest.getReplSetConfig();

    // Ensure the first will be primary.
    conf.members[0].priority = 1;
    conf.members[1].priority = 0;

    let primary = nodes[0];
    let secondary = nodes[1];
    let primaryDb = primary.getDB("admin");
    let secondaryDb = secondary.getDB("admin");

    clearRawMongoProgramOutput();

    // Verify that the secondary is up.
    assert.commandWorked(secondaryDb.runCommand({ping: 1}));

    // Manually initiate replica set because the initiate() command will hang
    // because the secondary will exit.
    let cmd = {replSetInitiate: conf};
    assert.commandWorked(primaryDb.runCommand(cmd), tojson(cmd));

    // Verify log messages are printed by the secondary.
    let msg1 = "IncompatibleServerVersion: Server min and max wire version are incompatible";
    // Don't check exact link because it will change.
    let msg2 = "Please consult the documentation for upgrading this server: ";
    assert.soon(
        function() {
            return rawMongoProgramOutput().match(msg1) && rawMongoProgramOutput().match(msg2);
        },
        "Secondary should have printed incompatible wire version and referenced documentation on exit",
        10 * 1000);

    // Wait for the secondary to stop responding.
    assert.soon(function() {
        try {
            secondaryDb.runCommand({ping: 1});
        } catch (e) {
            return true;
        }
    }, "Secondary should have stopped responding due to exit", 10 * 1000);

    // Secondary should only exit with the EXIT_NEED_UPGRADE error code.
    replTest.stop(secondary, undefined, {allowedExitCode: MongoRunner.EXIT_NEED_UPGRADE});
    replTest.stopSet();

})();
