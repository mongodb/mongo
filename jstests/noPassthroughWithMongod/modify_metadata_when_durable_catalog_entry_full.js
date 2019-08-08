/**
 * Tests that basic operations can be performed while the metadata document in the catalog is at the
 * size of the BSON document limit.
 */
(function() {
'use strict';

const dbName = 'modify_metadata_when_full';
const testDB = db.getSiblingDB(dbName);

/**
 * Returns the largest size of a collection name length possible where creating a collection with
 * one more character would fail.
 */
function createUntilFails(db, startingSize, increment) {
    if (increment == 1) {
        let newSize = startingSize;
        let collName = 'a'.repeat(newSize);

        while (db.createCollection(collName).ok) {
            assert.eq(true, db.getCollection(collName).drop());

            newSize++;
            collName = 'a'.repeat(newSize);
        }

        // Subtract one to get the largest possible collection name length.
        newSize -= 1;
        return newSize;
    }

    let newSize = startingSize + increment;
    let collName = 'a'.repeat(newSize);

    let res = db.createCollection(collName);
    if (res.ok) {
        assert.eq(true, db.getCollection(collName).drop());
        return createUntilFails(db, newSize, increment);
    } else {
        assert.eq("BSONObjectTooLarge", res.codeName);
        return createUntilFails(db, startingSize, Math.floor(increment / 2));
    }
}

/*
 * Creates the largest possible collection in terms of name length and returns it.
 */
function createLargeCollection(db) {
    // Divide by two because 'ns' field is stored twice in the catalog.
    const maxBsonObjectSize = db.isMaster().maxBsonObjectSize / 2;
    let maxCollNameSize = maxBsonObjectSize;
    let maxCollName = 'a'.repeat(maxCollNameSize);

    assert.commandWorked(testDB.createCollection(maxCollName));
    assert.eq(true, testDB.getCollection(maxCollName).drop());

    maxCollNameSize = createUntilFails(db, maxCollNameSize, 1000);
    maxCollName = 'b'.repeat(maxCollNameSize);

    // Creating a collection with an extra character should fail.
    let nameTooBig = 'c'.repeat(maxCollNameSize + 1);
    assert.commandFailedWithCode(testDB.createCollection(nameTooBig),
                                 ErrorCodes.BSONObjectTooLarge);

    // Create and return the collection with the largest possible name.
    assert.commandWorked(testDB.createCollection(maxCollName));
    return testDB.getCollection(maxCollName);
}

let largeColl = createLargeCollection(testDB);

// Ensure creating another collection works.
let smallCollName = 'd'.repeat(10000);
assert.commandWorked(testDB.createCollection(smallCollName));
let smallColl = testDB.getCollection(smallCollName);

// The 'top' command should fail because the response would be too big to return.
assert.commandFailedWithCode(largeColl.getDB().adminCommand('top'), 13548);

// Adding indexes to the large collection should fail but not crash the server.
assert.commandFailedWithCode(largeColl.createIndex({x: 1}), ErrorCodes.BSONObjectTooLarge);
assert.commandWorked(smallColl.createIndex({x: 1}));

// Inserting documents should work because it doesn't interact with the metadata document in the
// catalog.
assert.commandWorked(largeColl.insertMany([{x: 1}, {y: 2}, {z: 3}]));
assert.commandWorked(smallColl.insertMany([{x: 1}, {y: 2}, {z: 3}]));

// Renaming the collection should work to get ourselves out of situations where the collection name
// is too long for operations.
let largeCollName = largeColl.getFullName();
let adminDB = db.getSiblingDB('admin');
assert.commandWorked(adminDB.runCommand(
    {renameCollection: largeCollName, to: 'modify_metadata_when_full.smallName'}));
assert.commandWorked(adminDB.runCommand(
    {renameCollection: 'modify_metadata_when_full.smallName', to: largeCollName}));

// Renaming the small collection should fail because it has one more index than the large
// collection.
let otherLargeCollName = 'modify_metadata_when_full.' +
    'e'.repeat(largeColl.getName().length);
assert.commandFailedWithCode(
    adminDB.runCommand({renameCollection: smallColl.getFullName(), to: otherLargeCollName}),
    ErrorCodes.BSONObjectTooLarge);

// Dropping both collections should work.
assert.eq(true, largeColl.drop());
assert.eq(true, smallColl.drop());
}());
