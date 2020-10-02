/**
 * Test TTL docs are not deleted from secondaries directly
 * @tags: [requires_replication]
 */

// Skip db hash check because secondary will have extra document due to the usage of the godinsert
// command.
TestData.skipCheckDBHashes = true;

// setup set
const rt = new ReplSetTest({
    name: "ttl_repl",
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
const nodes = rt.startSet();
rt.initiate();
let primary = rt.getPrimary();
rt.awaitSecondaryNodes();
let secondary1 = rt.getSecondary();

// shortcuts
let primarydb = primary.getDB('d');
let secondary1db = secondary1.getDB('d');
let primarycol = primarydb['c'];
let secondary1col = secondary1db['c'];

// create TTL index, wait for TTL monitor to kick in, then check things
primarycol.ensureIndex({x: 1}, {expireAfterSeconds: 10});

rt.awaitReplication();

// increase logging
assert.commandWorked(secondary1col.getDB().adminCommand({setParameter: 1, logLevel: 1}));

// insert old doc (10 minutes old) directly on secondary using godinsert
assert.commandWorked(secondary1col.runCommand(
    "godinsert", {obj: {_id: new Date(), x: new Date((new Date()).getTime() - 600000)}}));
assert.eq(1, secondary1col.count(), "missing inserted doc");

sleep(70 * 1000);  // wait for 70seconds
assert.eq(1, secondary1col.count(), "ttl deleted my doc!");

// finish up
rt.stopSet();
