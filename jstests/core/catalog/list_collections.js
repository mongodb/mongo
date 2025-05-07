/*
 * Basic functional tests for the listCollections command.
 *
 * @tags: [
 *   # Cannot implicitly shard accessed collections because of collection existing when none
 *   # expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_getmore,
 *   requires_replication,
 *   uses_api_parameters,
 *   expects_explicit_underscore_id_index,
 * ]
 *
 * Note that storage engines used to be allowed to advertise internal collections to the user (in
 * particular, the MMAPv1 storage engine used to advertise the "system.indexes" collection).
 * Hence, this test suite does not test for a particular number of collections returned in
 * listCollections output, but rather tests for existence or absence of particular collections in
 * listCollections output.
 */

import {cursorCountMatching, getListCollectionsCursor} from 'jstests/core/catalog/libs/helpers.js';

const mydb = db.getSiblingDB("list_collections1");
let cursor;
let res;
let collObj;

jsTest.log('Test basic command output');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    res = mydb.runCommand("listCollections");
    assert.commandWorked(res);
    assert.eq('object', typeof (res.cursor));
    assert.eq(0, res.cursor.id);
    assert.eq('string', typeof (res.cursor.ns));
    collObj = res.cursor.firstBatch.filter(function(c) {
        return c.name === "foo";
    })[0];
    assert(collObj);
    assert.eq('object', typeof (collObj.options));
    assert.eq('collection', collObj.type, tojson(collObj));
    assert.eq(false, collObj.info.readOnly, tojson(collObj));
    assert.eq("object", typeof (collObj.idIndex), tojson(collObj));
    assert(collObj.idIndex.hasOwnProperty("v"), tojson(collObj));
}

jsTest.log('Test basic command output for views');
{
    assert.commandWorked(mydb.createView("bar", "foo", []));
    res = mydb.runCommand("listCollections");
    assert.commandWorked(res);
    collObj = res.cursor.firstBatch.filter(function(c) {
        return c.name === "bar";
    })[0];
    assert(collObj);
    assert.eq("object", typeof (collObj.options), tojson(collObj));
    assert.eq("foo", collObj.options.viewOn, tojson(collObj));
    assert.eq([], collObj.options.pipeline, tojson(collObj));
    assert.eq("view", collObj.type, tojson(collObj));
    assert.eq(true, collObj.info.readOnly, tojson(collObj));
    assert(!collObj.hasOwnProperty("idIndex"), tojson(collObj));
}

jsTest.log('Test basic usage with DBCommandCursor');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(mydb), function(c) {
                  return c.name === "foo";
              }));
}

jsTest.log('Test basic usage of "filter" option');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    assert.eq(2, cursorCountMatching(getListCollectionsCursor(mydb, {filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(2, getListCollectionsCursor(mydb, {filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {name: /^foo$/}}).itcount());
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {name: /^bar$/}}).itcount());
    mydb.foo.drop();
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(mydb, {filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(0, getListCollectionsCursor(mydb, {filter: {name: /^foo$/}}).itcount());
    assert.eq(1, getListCollectionsCursor(mydb, {filter: {name: /^bar$/}}).itcount());
    mydb.bar.drop();
    assert.eq(0, cursorCountMatching(getListCollectionsCursor(mydb, {filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(0, getListCollectionsCursor(mydb, {filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(0, getListCollectionsCursor(mydb, {filter: {name: /^foo$/}}).itcount());
    assert.eq(0, getListCollectionsCursor(mydb, {filter: {name: /^bar$/}}).itcount());
}

jsTest.log('Test for invalid values of "filter"');
{
    assert.throws(function() {
        getListCollectionsCursor(mydb, {filter: {$invalid: 1}});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {filter: 0});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {filter: 'x'});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {filter: []});
    });
}

jsTest.log('Test basic usage of "cursor.batchSize" option');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: 2}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: 1}});
    assert.eq(1, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: 0}});
    assert.eq(0, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: NumberInt(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: NumberLong(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: Math.pow(2, 62)}});
    assert.lte(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
}

jsTest.log(
    'listCollections accepts an empty object for "cursor" (same as not specifying "cursor" at all)');
// We do not test for objsLeftInBatch() here, since the default batch size for this command
// is not specified.
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    cursor = getListCollectionsCursor(mydb, {cursor: {}});
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
}

jsTest.log('Test for invalid values of "cursor" and "cursor.batchSize"');
{
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: 0});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: 'x'});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: []});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: {foo: 1}});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: {batchSize: -1}});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: {batchSize: 'x'}});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: {batchSize: {}}});
    });
    assert.throws(function() {
        getListCollectionsCursor(mydb, {cursor: {batchSize: 2, foo: 1}});
    });
}

jsTest.log('Test more than 2 batches of results');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    assert.commandWorked(mydb.createCollection("baz"));
    assert.commandWorked(mydb.createCollection("quux"));
    cursor = getListCollectionsCursor(mydb, {cursor: {batchSize: 0}}, 2);
    assert.eq(0, cursor.objsLeftInBatch());
    assert(cursor.hasNext());
    assert.eq(2, cursor.objsLeftInBatch());
    cursor.next();
    assert(cursor.hasNext());
    assert.eq(1, cursor.objsLeftInBatch());
    cursor.next();
    assert(cursor.hasNext());
    assert.eq(2, cursor.objsLeftInBatch());
    cursor.next();
    assert(cursor.hasNext());
    assert.eq(1, cursor.objsLeftInBatch());
}

jsTest.log('Test that batches are limited to ~16 MB');
{
    assert.commandWorked(mydb.dropDatabase());
    const validator = {
        $jsonSchema: {
            bsonType: "object",
            properties: {
                stringWith4mbDescription:
                    {bsonType: "string", description: "x".repeat(3 * 1024 * 1024)},

            }
        }
    };

    // Each collection's info is about 3 MB; 4 collections fit in the first batch and 2 in the
    // second.
    const nCollections = 6;
    jsTestLog(`Creating ${nCollections} collections with huge validator objects....`);
    for (let i = 0; i < nCollections; i++) {
        assert.commandWorked(mydb.createCollection("collection_" + i, {validator: validator}));
    }
    jsTestLog(`Done creating ${nCollections} collections`);

    // Filter out resharding temporal collections, which may exist when the balancer is moving
    // collections in background.
    cursor =
        getListCollectionsCursor(mydb, {filter: {name: {$not: {$regex: /system\.resharding/}}}});
    assert(cursor.hasNext());
    const firstBatchSize = cursor.objsLeftInBatch();
    assert.gt(firstBatchSize, 0);
    assert.lt(firstBatchSize, nCollections);
    // Exhaust the first batch..
    while (cursor.objsLeftInBatch()) {
        cursor.next();
    }
    assert(cursor.hasNext());
    const secondBatchSize = cursor.objsLeftInBatch();
    assert.eq(firstBatchSize + secondBatchSize, nCollections);
}

//
// Test on non-existent database.
//
jsTest.log('Test on non-existent database');
{
    assert.commandWorked(mydb.dropDatabase());
    cursor = getListCollectionsCursor(mydb);
    assert.eq(0, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo";
              }));
}

jsTest.log('Test on empty database');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    mydb.foo.drop();
    cursor = getListCollectionsCursor(mydb);
    assert.eq(0, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo";
              }));
}

jsTest.log('Test killCursors against a listCollections cursor');
{
    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    assert.commandWorked(mydb.createCollection("baz"));
    assert.commandWorked(mydb.createCollection("quux"));

    res = mydb.runCommand("listCollections", {cursor: {batchSize: 0}});
    cursor = new DBCommandCursor(mydb, res, 2);
    cursor.close();
    cursor = new DBCommandCursor(mydb, res, 2);
    assert.throws(function() {
        cursor.hasNext();
    });
}
