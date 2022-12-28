//  Tests to verify the behavior of find command's project in the presence of collation.
//
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

(function() {
'use strict';

const collation = {
    locale: "en_US",
    strength: 2
};
const withCollationCollName = jsTestName() + "_collation";
const noCollationCollName = jsTestName() + "_noCollation";

function setupCollection(withCollation) {
    const insertCollName = withCollation ? withCollationCollName : noCollationCollName;
    db[insertCollName].drop();
    if (withCollation) {
        assert.commandWorked(db.createCollection(insertCollName, {collation: withCollation}));
    }

    const insertColl = db[insertCollName];
    assert.commandWorked(insertColl.insert(
        {_id: 0, str: "a", array: [{str: "b"}, {str: "A"}, {str: "B"}, {str: "a"}]}));
    assert.commandWorked(insertColl.insert({_id: 1, str: "a", elemMatch: [{str: "A"}, "ignored"]}));
    assert.commandWorked(insertColl.insert({_id: 2, str: "A", elemMatch: ["ignored", {str: "a"}]}));
    assert.commandWorked(insertColl.insert({_id: 3, str: "B"}));

    return insertColl;
}

function runQueryWithCollation(testColl, collationToUse) {
    let findCmd =
        testColl.find({str: 'A', elemMatch: {$elemMatch: {str: "a"}}}, {_id: 1, 'elemMatch.$': 1});
    if (collationToUse) {
        findCmd = findCmd.collation(collationToUse);
    }
    const elemMatchOutput = findCmd.toArray();
    assert.sameMembers(elemMatchOutput,
                       [{_id: 1, elemMatch: [{str: "A"}]}, {_id: 2, elemMatch: [{str: "a"}]}]);

    findCmd =
        testColl.find({str: 'A'}, {sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}});
    if (collationToUse) {
        findCmd = findCmd.collation(collationToUse);
    }
    const sortArrayOutput = findCmd.toArray();
    assert.sameMembers(sortArrayOutput, [
        {_id: 0, sortedArray: [{str: "A"}, {str: "a"}, {str: "b"}, {str: "B"}]},
        {_id: 1, sortedArray: null},
        {_id: 2, sortedArray: null}
    ]);

    const findAndUpdateOutput = testColl.findAndModify({
        query: {_id: 1, str: 'A', elemMatch: {$elemMatch: {str: "a"}}},
        fields: {_id: 1, 'elemMatch.$': 1, updated: 1},
        update: {$set: {updated: true}},
        collation: collationToUse
    });
    assert.docEq(findAndUpdateOutput, {_id: 1, elemMatch: [{str: "A"}]});

    const findAndUpdateWithSortArrayOutput = testColl.findAndModify({
        query: {_id: 0, str: 'A'},
        fields: {_id: 1, str: 1, sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}},
        update: {$set: {updated: true}},
        collation: collationToUse,
        new: true
    });
    assert.docEq(findAndUpdateWithSortArrayOutput,
                 {_id: 0, str: "a", sortedArray: [{str: "A"}, {str: "a"}, {str: "b"}, {str: "B"}]});

    const findAndRemoveOutput = testColl.findAndModify({
        query: {_id: 1, str: 'A', elemMatch: {$elemMatch: {str: "a"}}},
        fields: {_id: 1, 'elemMatch.$': 1, updated: 1},
        remove: true,
        collation: collationToUse,
    });
    assert.docEq(findAndRemoveOutput, {_id: 1, elemMatch: [{str: "A"}], updated: true});

    const findAndRemoveWithSortArrayOutput = testColl.findAndModify({
        query: {_id: 0, str: 'A'},
        fields: {_id: 1, str: 1, sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}},
        remove: true,
        collation: collationToUse
    });
    assert.docEq(findAndRemoveWithSortArrayOutput,
                 {_id: 0, str: "a", sortedArray: [{str: "A"}, {str: "a"}, {str: "b"}, {str: "B"}]});
}

// The output for the below two tests should not depend on the collection level collation.
let collWithCollation = setupCollection({locale: "en_US"});
runQueryWithCollation(collWithCollation, collation);

let noCollationColl = setupCollection(false);
runQueryWithCollation(noCollationColl, collation);

// Tests to verify that the projection code inherits collection level collation in the absence of
// query level collation.
collWithCollation = setupCollection(collation);
runQueryWithCollation(collWithCollation, null);

// The output of this should not depend on the collection level collation and simple collation
// should be applied always.
function queryWithSimpleCollation(testColl) {
    const elemMatchOutput =
        testColl.find({str: 'A', elemMatch: {$elemMatch: {str: "a"}}}, {_id: 1, 'elemMatch.$': 1})
            .collation({locale: "simple"})
            .toArray();
    assert.sameMembers(elemMatchOutput, [{_id: 2, elemMatch: [{str: "a"}]}]);

    const sortArrayOutput =
        testColl.find({str: 'a'}, {sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}})
            .collation({locale: "simple"})
            .toArray();
    assert.sameMembers(sortArrayOutput, [
        {_id: 0, sortedArray: [{str: "A"}, {str: "B"}, {str: "a"}, {str: "b"}]},
        {_id: 1, sortedArray: null}
    ]);

    // Test findAndModify command with 'update'. Ensure that simple collation is always honored.
    const findAndUpdateOutput = testColl.findAndModify({
        query: {str: 'A', elemMatch: {$elemMatch: {str: "a"}}},
        fields: {_id: 1, 'elemMatch.$': 1, updated: 1},
        update: {$set: {updated: true}},
        collation: {locale: "simple"}
    });
    assert.docEq(findAndUpdateOutput, {_id: 2, elemMatch: [{str: "a"}]});

    const findAndUpdateWithSortArrayOutput = testColl.findAndModify({
        query: {_id: 0, str: 'a'},
        fields: {_id: 1, str: 1, sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}},
        update: {$set: {updated: true}},
        collation: {locale: "simple"},
        new: true
    });
    assert.docEq(findAndUpdateWithSortArrayOutput,
                 {_id: 0, str: "a", sortedArray: [{str: "A"}, {str: "B"}, {str: "a"}, {str: "b"}]});

    // Test findAndModify command with remove:true. Ensure that simple collation is always honored.
    const findAndRemoveOutput = testColl.findAndModify({
        query: {_id: 2, str: 'A', elemMatch: {$elemMatch: {str: "a"}}},
        fields: {_id: 1, 'elemMatch.$': 1, updated: 1},
        remove: true,
        collation: {locale: "simple"},
    });
    assert.docEq(findAndRemoveOutput, {_id: 2, elemMatch: [{str: "a"}], updated: true});

    const findAndRemoveWithSortArrayOutput = testColl.findAndModify({
        query: {_id: 0, str: 'a'},
        fields: {_id: 1, str: 1, sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}},
        remove: true,
        collation: {locale: "simple"}
    });
    assert.docEq(findAndRemoveWithSortArrayOutput,
                 {_id: 0, str: "a", sortedArray: [{str: "A"}, {str: "B"}, {str: "a"}, {str: "b"}]});
}

noCollationColl = setupCollection(false);
queryWithSimpleCollation(noCollationColl);

collWithCollation = setupCollection(collation);
queryWithSimpleCollation(collWithCollation);

// Test with views.
(function viewWithCollation() {
    collWithCollation = setupCollection(collation);
    db[jsTestName() + "_view"].drop();
    assert.commandWorked(
        db.createView(jsTestName() + "_view", withCollationCollName, [], {collation: collation}));
    const viewColl = db[jsTestName() + "_view"];

    const sortArrayOutput =
        viewColl.find({str: 'A'}, {sortedArray: {$sortArray: {input: "$array", sortBy: {str: 1}}}})
            .toArray();
    assert.sameMembers(sortArrayOutput, [
        {_id: 0, sortedArray: [{str: "A"}, {str: "a"}, {str: "b"}, {str: "B"}]},
        {_id: 1, sortedArray: null},
        {_id: 2, sortedArray: null}
    ]);
})();
})();
