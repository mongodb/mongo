/**
 * Test running the 'captrunc' command on various kinds of collections:
 *   - indexed capped collections
 *   - nonexistent collections
 *   - non-capped collections
 *
 * This test fails with the ephemeralForTest storage engine.
 * @tags: [SERVER-21658]
 */
(function() {
    'use strict';

    db.capped_truncate.drop();
    assert.commandWorked(
        db.runCommand({create: "capped_truncate", capped: true, size: 1000, autoIndexId: true}));
    var t = db.capped_truncate;

    // It is an error to remove a non-positive number of documents.
    assert.commandFailed(db.runCommand({captrunc: "capped_truncate", n: -1}),
                         "captrunc didn't return an error when attempting to remove a negative " +
                             "number of documents");
    assert.commandFailed(db.runCommand({captrunc: "capped_truncate", n: 0}),
                         "captrunc didn't return an error when attempting to remove 0 documents");

    for (var j = 1; j <= 10; j++) {
        assert.writeOK(t.insert({x: j}));
    }

    // It is an error to try and remove more documents than what exist in the capped collection.
    assert.commandFailed(db.runCommand({captrunc: "capped_truncate", n: 20}),
                         "captrunc didn't return an error when attempting to remove more" +
                             " documents than what the collection contains");

    assert.commandWorked(db.runCommand({captrunc: "capped_truncate", n: 5, inc: false}));
    assert.eq(5, t.count(), "wrong number of documents in capped collection after truncate");
    assert.eq(5, t.distinct("_id").length, "wrong number of entries in _id index after truncate");

    var last = t.find({}, {_id: 1}).sort({_id: -1}).next();
    assert.neq(null,
               t.findOne({_id: last._id}),
               tojson(last) + " is in _id index, but not in capped collection after truncate");

    // It is an error to run the captrunc command on a nonexistent collection.
    assert.commandFailed(db.runCommand({captrunc: "nonexistent", n: 1}),
                         "captrunc didn't return an error for a nonexistent collection");

    // It is an error to run the captrunc command on a non-capped collection.
    var collName = "noncapped";
    db[collName].drop();

    assert.commandWorked(db.runCommand({create: collName, capped: false}));
    for (var j = 1; j <= 10; j++) {
        assert.writeOK(db[collName].insert({x: j}));
    }
    assert.commandFailed(db.runCommand({captrunc: collName, n: 5}),
                         "captrunc didn't return an error for a non-capped collection");
})();
