// This is a regression test for SERVER-32048. It checks that index keys are correctly updated when
// an update modifier implicitly creates a new array element.
(function() {
    "use strict";

    let coll = db.update_affects_indexes;
    coll.drop();
    let indexKeyPattern = {"a.b": 1};
    assert.commandWorked(coll.createIndex(indexKeyPattern));

    // Tests that the document 'docId' has all the index keys in 'expectedKeys' and none of the
    // index keys in 'unexpectedKeys'.
    function assertExpectedIndexKeys(docId, expectedKeys, unexpectedKeys) {
        for (let key of expectedKeys) {
            let res = coll.find(docId).hint(indexKeyPattern).min(key).returnKey().toArray();
            assert.eq(1, res.length, tojson(res));
            assert.eq(key, res[0]);
        }

        for (let key of unexpectedKeys) {
            let res = coll.find(docId).hint(indexKeyPattern).min(key).returnKey().toArray();
            if (res.length > 0) {
                assert.eq(1, res.length, tojson(res));
                assert.neq(key, res[0]);
            }
        }
    }

    // $set implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 0, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 0}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 0}, {$set: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 0}, [{"a.b": 0}, {"a.b": null}], []);

    // $set implicitly creates array element beyond end of array.
    assert.writeOK(coll.insert({_id: 1, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 1}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 1}, {$set: {"a.3.c": 0}}));
    assertExpectedIndexKeys({_id: 1}, [{"a.b": 0}, {"a.b": null}], []);

    // $set implicitly creates array element in empty array (no index key changes needed).
    assert.writeOK(coll.insert({_id: 2, a: []}));
    assertExpectedIndexKeys({_id: 2}, [{"a.b": null}], []);
    assert.writeOK(coll.update({_id: 2}, {$set: {"a.0.c": 0}}));
    assertExpectedIndexKeys({_id: 2}, [{"a.b": null}], []);

    // $inc implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 3, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 3}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 3}, {$inc: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 3}, [{"a.b": 0}, {"a.b": null}], []);

    // $mul implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 4, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 4}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 4}, {$mul: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 4}, [{"a.b": 0}, {"a.b": null}], []);

    // $addToSet implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 5, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 5}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 5}, {$addToSet: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 5}, [{"a.b": 0}, {"a.b": null}], []);

    // $bit implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 6, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 6}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 6}, {$bit: {"a.1.c": {and: NumberInt(1)}}}));
    assertExpectedIndexKeys({_id: 6}, [{"a.b": 0}, {"a.b": null}], []);

    // $min implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 7, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 7}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 7}, {$min: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 7}, [{"a.b": 0}, {"a.b": null}], []);

    // $max implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 8, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 8}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 8}, {$max: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 8}, [{"a.b": 0}, {"a.b": null}], []);

    // $currentDate implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 9, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 9}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 9}, {$currentDate: {"a.1.c": true}}));
    assertExpectedIndexKeys({_id: 9}, [{"a.b": 0}, {"a.b": null}], []);

    // $push implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 10, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 10}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 10}, {$push: {"a.1.c": 0}}));
    assertExpectedIndexKeys({_id: 10}, [{"a.b": 0}, {"a.b": null}], []);

    // $pushAll implicitly creates array element at end of array.
    assert.writeOK(coll.insert({_id: 11, a: [{b: 0}]}));
    assertExpectedIndexKeys({_id: 11}, [{"a.b": 0}], [{"a.b": null}]);
    assert.writeOK(coll.update({_id: 11}, {$pushAll: {"a.1.c": [0]}}));
    assertExpectedIndexKeys({_id: 11}, [{"a.b": 0}, {"a.b": null}], []);
}());
