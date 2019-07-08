/**
 * Test that the 'findAndModify' command throws the expected errors for invalid query, sort and
 * projection parameters. This test exercises the fix for SERVER-41829.
 * @tags: [assumes_unsharded_collection]
 */
(function() {
    "use strict";

    const coll = db.find_and_modify_invalid_inputs;
    coll.drop();
    coll.insert({_id: 0});
    coll.insert({_id: 1});

    function assertFailedWithCode(cmd, errorCode) {
        const err = assert.throws(() => coll.findAndModify(cmd));
        assert.eq(err.code, errorCode);
    }

    function assertWorked(cmd, expectedValue) {
        const out = assert.doesNotThrow(() => coll.findAndModify(cmd));
        assert.eq(out.value, expectedValue);
    }

    // Verify that the findAndModify command works when we supply a valid query.
    let out = coll.findAndModify({query: {_id: 1}, update: {$set: {value: "basic"}}, new: true});
    assert.eq(out, {_id: 1, value: "basic"});

    // Verify that invalid 'query' object fails.
    assertFailedWithCode({query: null, update: {value: 2}}, 31160);
    assertFailedWithCode({query: 1, update: {value: 2}}, 31160);
    assertFailedWithCode({query: "{_id: 1}", update: {value: 2}}, 31160);
    assertFailedWithCode({query: false, update: {value: 2}}, 31160);

    // Verify that missing and empty query object is allowed.
    assertWorked({update: {$set: {value: "missingQuery"}}, new: true}, "missingQuery");
    assertWorked({query: {}, update: {$set: {value: "emptyQuery"}}, new: true}, "emptyQuery");

    // Verify that command works when we supply a valid sort specification.
    assertWorked({sort: {_id: -1}, update: {$set: {value: "sort"}}, new: true}, "sort");

    // Verify that invaid 'sort' object fails.
    assertFailedWithCode({sort: null, update: {value: 2}}, 31174);
    assertFailedWithCode({sort: 1, update: {value: 2}}, 31174);
    assertFailedWithCode({sort: "{_id: 1}", update: {value: 2}}, 31174);
    assertFailedWithCode({sort: false, update: {value: 2}}, 31174);

    // Verify that missing and empty 'sort' object is allowed.
    assertWorked({update: {$set: {value: "missingSort"}}, new: true}, "missingSort");
    assertWorked({sort: {}, update: {$set: {value: "emptySort"}}, new: true}, "emptySort");

    // Verify that the 'fields' projection works.
    assertWorked({fields: {_id: 0}, update: {$set: {value: "project"}}, new: true}, "project");

    // Verify that invaid 'fields' object fails.
    assertFailedWithCode({fields: null, update: {value: 2}}, 31175);
    assertFailedWithCode({fields: 1, update: {value: 2}}, 31175);
    assertFailedWithCode({fields: "{_id: 1}", update: {value: 2}}, 31175);
    assertFailedWithCode({fields: false, update: {value: 2}}, 31175);

    // Verify that missing and empty 'fields' object is allowed. Also verify that the command
    // projects all the fields.
    assertWorked({update: {$set: {value: "missingFields"}}, new: true}, "missingFields");
    assertWorked({fields: {}, update: {$set: {value: "emptyFields"}}, new: true}, "emptyFields");

    // Verify that findOneAndDelete() shell helper throws the same errors as findAndModify().
    let err = assert.throws(() => coll.findOneAndDelete("{_id: 1}"));
    assert.eq(err.code, 31160);
    err = assert.throws(() => coll.findOneAndDelete(null, {sort: 1}));
    assert.eq(err.code, 31174);

    // Verify that findOneAndReplace() shell helper throws the same errors as findAndModify().
    err = assert.throws(() => coll.findOneAndReplace("{_id: 1}", {}));
    assert.eq(err.code, 31160);
    err = assert.throws(() => coll.findOneAndReplace(null, {}, {sort: 1}));
    assert.eq(err.code, 31174);

    // Verify that findOneAndUpdate() shell helper throws the same errors as findAndModify().
    err = assert.throws(() => coll.findOneAndUpdate("{_id: 1}", {$set: {value: "new"}}));
    assert.eq(err.code, 31160);
    err = assert.throws(() => coll.findOneAndUpdate(null, {$set: {value: "new"}}, {sort: 1}));
    assert.eq(err.code, 31174);

    // Verify that find and modify shell helpers allow null query object.
    out =
        coll.findOneAndUpdate(null, {$set: {value: "findOneAndUpdate"}}, {returnNewDocument: true});
    assert.eq(out.value, "findOneAndUpdate");

    out = coll.findOneAndReplace(null, {value: "findOneAndReplace"}, {returnNewDocument: true});
    assert.eq(out.value, "findOneAndReplace");

    out = coll.findOneAndDelete(null);
    assert.eq(out.value, "findOneAndReplace");
})();
