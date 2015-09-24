// Ensure that findAndModify returns a user error if the {_id: 1} index is missing entries.
// Regression test for SERVER-20531.
(function() {
    'use strict';

    var mongo = MongoRunner.runMongod({setParameter: "failIndexKeyTooLong=false"});
    var testDB = mongo.getDB("test");
    var coll = testDB.fam_longindexkey;
    coll.drop();

    var MAX_KEY_LENGTH = 1024;
    var longKey = new Array(MAX_KEY_LENGTH + 2).join("x");
    assert.eq(MAX_KEY_LENGTH + 1, longKey.length);

    assert.writeOK(coll.insert({_id: longKey, a: 0}));

    // Should fail because the long _id is omitted from the _id index.
    assert.throws(function() { coll.findAndModify({query: {a: 0}, update: {$set: {a: 1}}}); });

    // Ensure that the server is still up and running.
    assert.eq(1, coll.find().itcount());

    // The update will not have happened.
    assert.eq(0, coll.findOne()["a"]);
})();
