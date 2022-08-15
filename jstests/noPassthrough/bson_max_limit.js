/**
 * Tests that the server accepts writes that push a bson object to the documented maximum size
 * limit.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isReplSet().

function retryableFindAndModify(database, collection, query, update, newImage, upsert, remove) {
    let cmd = {
        findAndModify: collection,
        query: query,
        lsid: {id: UUID()},
        txnNumber: NumberLong(1),
    };
    if (update) {
        cmd["update"] = update;
    }
    if (newImage) {
        cmd["new"] = newImage;
    }
    if (upsert) {
        cmd["upsert"] = upsert;
    }
    if (remove) {
        cmd["remove"] = remove;
    }

    assert.commandWorked(database.runCommand(cmd));
}

function executeTest(db) {
    const MaxBsonSize = db.hello().maxBsonObjectSize;
    let doc = {"_id": 1, "a": ""};
    let leftoverSpace = MaxBsonSize - Object.bsonsize(doc);
    let bigStr = "a".repeat(leftoverSpace);
    doc["a"] = bigStr;
    jsTestLog({"Max bson size": MaxBsonSize, "Doc Size": Object.bsonsize(doc)});
    assert.eq(Object.bsonsize(doc), MaxBsonSize);

    db["coll"].drop();

    // Assert inserting at the MaxBsonSize works.
    assert.commandWorked(db["coll"].insert(doc));
    // Assert a size increasing update up to the MaxBsonSize works.
    assert.commandWorked(db["coll"].update({_id: 1}, {$unset: {a: 1}}));
    assert.commandWorked(db["coll"].update({_id: 1}, {$set: {a: bigStr}}));
    // Assert replacing a MaxBsonSize object with another MaxBsonSize object works.
    assert.commandWorked(db["coll"].update({_id: 1}, {$set: {a: bigStr}}));

    // Reset
    assert.commandWorked(db["coll"].remove({}));
    // Assert inserting at the MaxBsonSize works.
    assert.commandWorked(db["coll"].insert(doc));
    // Assert a size increasing update up to the MaxBsonSize works.
    assert.commandWorked(db["coll"].update({_id: 1}, {$unset: {a: 1}}));
    assert.commandWorked(db["coll"].update({_id: 1}, {$set: {a: bigStr}}));
    // Assert replacing a MaxBsonSize object with another MaxBsonSize object works.
    assert.commandWorked(db["coll"].update({_id: 1}, {$set: {a: bigStr}}));

    if (!FixtureHelpers.isReplSet(db)) {
        return;
    }

    // Reset. Test retryable findAndModify's.
    let sessionDb = db.getMongo().startSession({}).getDatabase("test");
    retryableFindAndModify(db, "coll", {_id: 1}, false, false, false, /*remove=*/true);
    retryableFindAndModify(db, "coll", {_id: 1}, doc, false, /*upsert=*/true, false);
    retryableFindAndModify(db, "coll", {_id: 1}, {$unset: {a: 1}}, false, false, false);
    retryableFindAndModify(
        db, "coll", {_id: 1}, {$set: {a: bigStr}}, /*new(Image)=*/true, false, false);
}

{
    // Run against a standalone
    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");
    executeTest(conn.getDB("test"));
    MongoRunner.stopMongod(conn);
}

{
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
                // Disallow elections on secondary.
                rsConfig: {
                    priority: 0,
                    votes: 0,
                },
            }
        ]
    });
    rst.startSet();
    rst.initiate();
    // Test the modern default behavior where storeFindAndModifyImagesInSideCollection is true.
    rst.getPrimary().adminCommand(
        {setParameter: 1, storeFindAndModifyImagesInSideCollection: true});
    executeTest(rst.getPrimary().getDB("test"));
    rst.stopSet();
}
})();
