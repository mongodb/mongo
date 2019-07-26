/**
 * Drops all sharded collections (except for collections used internally,
 * like config.system.sessions).
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isMongos.

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');
assert(FixtureHelpers.isMongos(db), "not connected to mongos");

let balSettingResult = assert.commandWorked(db.adminCommand({balancerStatus: 1}));
if (balSettingResult.mode !== 'off') {
    assert.commandWorked(db.adminCommand({balancerStop: 1}));
}

db.getSiblingDB('config').collections.find().forEach(collEntry => {
    if (collEntry._id !== 'config.system.sessions') {
        let nsSplit = collEntry._id.split('.');
        const dbName = nsSplit.shift();
        const collName = nsSplit.join('.');

        // Note: drop also cleans up tags and chunks associated with ns.
        assert.commandWorked(db.getSiblingDB(dbName).runCommand({drop: collName}));
    }
});

// Turn balancer back on if it was not off earlier.
if (balSettingResult.mode !== 'off') {
    assert.commandWorked(db.adminCommand({balancerStart: 1}));
}
})();
