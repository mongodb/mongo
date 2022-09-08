/**
 * Tests that the cluster cannot be downgraded when there are capped collections with a size that
 * is non multiple of 256 bytes. The user has to resize or drop the collection in order to
 * downgrade.
 */
(function() {

const conn = MongoRunner.runMongod();
const testDB = conn.getDB(jsTestName());
const cappedColl = testDB["capped_coll"];
const options = Object.assign({}, {capped: true}, {size: 50 * 1023});
testDB.createCollection(cappedColl.getName(), options);

// We expect the server to be in a non-downgradable state initially and "command" is what we have to
// run to correct the state in order to successfully downgrade.
function checkCappedCollectionForDowngrade(command) {
    assert.commandFailedWithCode(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);
    testDB.runCommand(command);
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}

// We want to resize the collection to have a size multiple of 256 bytes in order to be able to
// downgrade.
const resizeCommand = Object.assign({}, {collMod: cappedColl.getName()}, {cappedSize: 50 * 1024});
checkCappedCollectionForDowngrade(resizeCommand);

// We reset the size of the collection to be a non multiple of 256 bytes.
const resetSizeCommand =
    Object.assign({}, {collMod: cappedColl.getName()}, {cappedSize: 50 * 1023});
testDB.runCommand(resetSizeCommand);

// We want to drop the collection in order to be able to downgrade.
const dropCommand = Object.assign({}, {drop: cappedColl.getName()});
checkCappedCollectionForDowngrade(dropCommand);

MongoRunner.stopMongod(conn);
}());
