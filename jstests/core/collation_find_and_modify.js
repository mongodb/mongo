// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

// Integration tests for collation-aware findAndModify.
(function() {
    'use strict';
    var coll = db.getCollection("find_and_modify_update_test");

    const caseInsensitive = {locale: "en_US", strength: 2};
    const caseSensitive = {locale: "en_US", strength: 3};

    // We restrict testing pipeline-style update to commands as they are not supported for OP_UPDATE
    // which cannot differentiate an update object from an array.
    if (db.getMongo().writeMode() === "commands") {
        //
        // Pipeline-style update respects collection default collation.
        //

        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {collation: caseInsensitive}));
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        let doc = coll.findAndModify(
            {update: [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}], new: true});
        assert.eq(doc.newField, 3, doc);

        //
        // Pipeline-style findAndModify respects query collation.
        //

        // Case sensitive $indexOfArray on "B" matches "B".
        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}],
            collation: caseSensitive,
            new: true
        });
        assert.eq(doc.newField, 5, doc);

        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$project: {newField: {$indexOfArray: ["$x", "B"]}}}],
            collation: caseSensitive,
            new: true
        });
        assert.eq(doc.newField, 5, doc);

        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$replaceRoot: {newRoot: {newField: {$indexOfArray: ["$x", "B"]}}}}],
            collation: caseSensitive,
            new: true
        });
        assert.eq(doc.newField, 5, doc);

        // Case insensitive $indexOfArray on "B" matches "b".
        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}],
            collation: caseInsensitive,
            new: true
        });
        assert.eq(doc.newField, 3, doc);

        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$project: {newField: {$indexOfArray: ["$x", "B"]}}}],
            collation: caseInsensitive,
            new: true
        });
        assert.eq(doc.newField, 3, doc);

        assert(coll.drop());
        assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
        doc = coll.findAndModify({
            update: [{$replaceRoot: {newRoot: {newField: {$indexOfArray: ["$x", "B"]}}}}],
            collation: caseInsensitive,
            new: true
        });
        assert.eq(doc.newField, 3, doc);
    }
})();
