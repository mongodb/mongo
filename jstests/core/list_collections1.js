// Basic functional tests for the listCollections command.
//
// Note that storage engines are allowed to advertise internal collections to the user (in
// particular, the MMAPv1 storage engine currently advertises the "system.indexes" collection).
// Hence, this test suite does not test for a particular number of collections returned in
// listCollections output, but rather tests for existence or absence of particular collections in
// listCollections output.

(function() {
    "use strict";

    var mydb = db.getSiblingDB("list_collections1");
    var cursor;
    var res;
    var collObj;

    //
    // Test basic command output.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    res = mydb.runCommand("listCollections");
    assert.commandWorked(res);
    assert.eq('object', typeof(res.cursor));
    assert.eq(0, res.cursor.id);
    assert.eq('string', typeof(res.cursor.ns));
    collObj = res.cursor.firstBatch.filter(function(c) {
        return c.name === "foo";
    })[0];
    assert(collObj);
    assert.eq('object', typeof(collObj.options));
    assert.eq('collection', collObj.type, tojson(collObj));
    assert.eq(false, collObj.info.readOnly, tojson(collObj));

    //
    // Test basic usage with DBCommandCursor.
    //

    var getListCollectionsCursor = function(options, subsequentBatchSize) {
        return new DBCommandCursor(
            mydb.getMongo(), mydb.runCommand("listCollections", options), subsequentBatchSize);
    };

    var cursorCountMatching = function(cursor, pred) {
        return cursor.toArray().filter(pred).length;
    };

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(), function(c) {
                  return c.name === "foo";
              }));

    //
    // Test that the collection metadata object is returned correctly.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar", {temp: true}));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(), function(c) {
                  return c.name === "foo" && c.options.temp === undefined;
              }));
    assert.eq(1, cursorCountMatching(getListCollectionsCursor(), function(c) {
                  return c.name === "bar" && c.options.temp === true;
              }));

    //
    // Test basic usage of "filter" option.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar", {temp: true}));
    assert.eq(2, cursorCountMatching(getListCollectionsCursor({filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(2, getListCollectionsCursor({filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(1, getListCollectionsCursor({filter: {name: /^foo$/}}).itcount());
    assert.eq(1, getListCollectionsCursor({filter: {"options.temp": true}}).itcount());
    mydb.foo.drop();
    assert.eq(1, cursorCountMatching(getListCollectionsCursor({filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(1, getListCollectionsCursor({filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(0, getListCollectionsCursor({filter: {name: /^foo$/}}).itcount());
    assert.eq(1, getListCollectionsCursor({filter: {"options.temp": true}}).itcount());
    mydb.bar.drop();
    assert.eq(0, cursorCountMatching(getListCollectionsCursor({filter: {}}), function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    assert.eq(0, getListCollectionsCursor({filter: {name: {$in: ["foo", "bar"]}}}).itcount());
    assert.eq(0, getListCollectionsCursor({filter: {name: /^foo$/}}).itcount());
    assert.eq(0, getListCollectionsCursor({filter: {"options.temp": true}}).itcount());

    //
    // Test for invalid values of "filter".
    //

    assert.throws(function() {
        getListCollectionsCursor({filter: {$invalid: 1}});
    });
    assert.throws(function() {
        getListCollectionsCursor({filter: 0});
    });
    assert.throws(function() {
        getListCollectionsCursor({filter: 'x'});
    });
    assert.throws(function() {
        getListCollectionsCursor({filter: []});
    });

    //
    // Test basic usage of "cursor.batchSize" option.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    cursor = getListCollectionsCursor({cursor: {batchSize: 2}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor({cursor: {batchSize: 1}});
    assert.eq(1, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor({cursor: {batchSize: 0}});
    assert.eq(0, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    cursor = getListCollectionsCursor({cursor: {batchSize: NumberInt(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));
    cursor = getListCollectionsCursor({cursor: {batchSize: NumberLong(2)}});
    assert.eq(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    // Test a large batch size, and assert that at least 2 results are returned in the initial
    // batch.
    cursor = getListCollectionsCursor({cursor: {batchSize: Math.pow(2, 62)}});
    assert.lte(2, cursor.objsLeftInBatch());
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    // Ensure that the server accepts an empty object for "cursor".  This is equivalent to not
    // specifying "cursor" at all.
    //
    // We do not test for objsLeftInBatch() here, since the default batch size for this command
    // is not specified.
    cursor = getListCollectionsCursor({cursor: {}});
    assert.eq(2, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo" || c.name === "bar";
              }));

    //
    // Test for invalid values of "cursor" and "cursor.batchSize".
    //

    assert.throws(function() {
        getListCollectionsCursor({cursor: 0});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: 'x'});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: []});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: {foo: 1}});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: {batchSize: -1}});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: {batchSize: 'x'}});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: {batchSize: {}}});
    });
    assert.throws(function() {
        getListCollectionsCursor({cursor: {batchSize: 2, foo: 1}});
    });

    //
    // Test more than 2 batches of results.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    assert.commandWorked(mydb.createCollection("baz"));
    assert.commandWorked(mydb.createCollection("quux"));
    cursor = getListCollectionsCursor({cursor: {batchSize: 0}}, 2);
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

    //
    // Test on non-existent database.
    //

    assert.commandWorked(mydb.dropDatabase());
    cursor = getListCollectionsCursor();
    assert.eq(0, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo";
              }));

    //
    // Test on empty database.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    mydb.foo.drop();
    cursor = getListCollectionsCursor();
    assert.eq(0, cursorCountMatching(cursor, function(c) {
                  return c.name === "foo";
              }));

    //
    // Test killCursors against a listCollections cursor.
    //

    assert.commandWorked(mydb.dropDatabase());
    assert.commandWorked(mydb.createCollection("foo"));
    assert.commandWorked(mydb.createCollection("bar"));
    assert.commandWorked(mydb.createCollection("baz"));
    assert.commandWorked(mydb.createCollection("quux"));

    res = mydb.runCommand("listCollections", {cursor: {batchSize: 0}});
    cursor = new DBCommandCursor(mydb.getMongo(), res, 2);
    cursor = null;
    gc();  // Shell will send a killCursors message when cleaning up underlying cursor.
    cursor = new DBCommandCursor(mydb.getMongo(), res, 2);
    assert.throws(function() {
        cursor.hasNext();
    });
}());
