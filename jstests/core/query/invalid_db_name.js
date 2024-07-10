// Ensures that invalid DB names are reported as write errors
//
// Can't create a collection with invalid database name
// @tags: [
//     assumes_no_implicit_collection_creation_on_get_collection
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function testInvalidDBName(invalidName) {
    const isMongos = FixtureHelpers.isMongos(db);
    const errMsg = `'${invalidName}' database with invalid name was created`;
    let invalidDB = db.getSiblingDB("NonExistentDB");

    // This is a hack to bypass invalid database name checking by the DB constructor
    invalidDB._name = invalidName;

    function validateState() {
        // Ensure that no database was created
        var dbList = db.getSiblingDB('admin').runCommand({listDatabases: 1}).databases;
        dbList.forEach(function(dbInfo) {
            assert.neq(invalidName, dbInfo.name, errMsg);
        });

        // On sharding ensure no entry was added to config.databases
        if (isMongos) {
            assert.eq(
                db.getSiblingDB('config').databases.countDocuments({_id: invalidName}), 0, errMsg);
        }
    }

    function testCommandFailsAndValidate(command) {
        assert.commandFailedWithCode(command(), ErrorCodes.InvalidNamespace, errMsg);
        validateState();
    }

    function testWriteErrorAndValidate(command) {
        assert.writeError(command(), errMsg);
        validateState();
    }

    testWriteErrorAndValidate(() => {
        return invalidDB.test.insert({x: 1});
    });
    testCommandFailsAndValidate(() => {
        return invalidDB.createCollection("test");
    });
    testCommandFailsAndValidate(() => {
        return invalidDB.createView("a", "b", [{$match: {year: 1}}]);
    });
    if (isMongos) {
        testCommandFailsAndValidate(() => {
            return sh.shardCollection(invalidName + ".test", {a: 1});
        });
        testCommandFailsAndValidate(() => {
            return sh.enableSharding(invalidName);
        });
    }
}

testInvalidDBName("Invalid DB Name");
testInvalidDBName("$invalidDBName");
