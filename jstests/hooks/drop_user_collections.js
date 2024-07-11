/**
 * Drops all user collections.
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');
assert(FixtureHelpers.isMongos(db), "not connected to mongos");

let balSettingResult = assert.commandWorked(db.adminCommand({balancerStatus: 1}));
if (balSettingResult.mode !== 'off') {
    assert.commandWorked(db.adminCommand({balancerStop: 1}));
}

db.adminCommand({listDatabases: 1}).databases.forEach(dbEntry => {
    const dbName = dbEntry.name;
    if (dbName == 'admin' || dbName == 'config' || dbName == 'local')
        return;
    // This will drop collections and views (including timeseries collections).
    db.getSiblingDB(dbName).getCollectionNames().forEach(collName => {
        if (collName.startsWith('system.'))
            return;

        assert.commandWorked(db.getSiblingDB(dbName).runCommand({drop: collName}));
    });
});

// Turn balancer back on if it was not off earlier.
if (balSettingResult.mode !== 'off') {
    assert.commandWorked(db.adminCommand({balancerStart: 1}));
}
