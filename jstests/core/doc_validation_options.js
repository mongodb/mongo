(function() {
    "use strict";

    function assertFailsValidation(res) {
        var DocumentValidationFailure = 121;
        assert.writeError(res);
        assert.eq(res.getWriteError().code, DocumentValidationFailure);
    }

    var t = db.doc_validation_options;
    t.drop();

    assert.commandWorked(db.createCollection(t.getName(), {validator: {a: 1}}));

    assertFailsValidation(t.insert({a: 2}));
    t.insert({a: 1});
    assert.eq(1, t.count());

    // test default to strict
    assertFailsValidation(t.update({}, {$set: {a: 2}}));
    assert.eq(1, t.find({a: 1}).itcount());

    // check we can do a bad update in warn mode
    assert.commandWorked(t.runCommand("collMod", {validationAction: "warn"}));
    t.update({}, {$set: {a: 2}});
    assert.eq(1, t.find({a: 2}).itcount());

    // TODO: check log for message?

    // make sure persisted
    var info = db.getCollectionInfos({name: t.getName()})[0];
    assert.eq("warn", info.options.validationAction, tojson(info));

    // check we can go back to enforce strict
    assert.commandWorked(
        t.runCommand("collMod", {validationAction: "error", validationLevel: "strict"}));
    assertFailsValidation(t.update({}, {$set: {a: 3}}));
    assert.eq(1, t.find({a: 2}).itcount());

    // check bad -> bad is ok
    assert.commandWorked(t.runCommand("collMod", {validationLevel: "moderate"}));
    t.update({}, {$set: {a: 3}});
    assert.eq(1, t.find({a: 3}).itcount());

    // test create
    t.drop();
    assert.commandWorked(
        db.createCollection(t.getName(), {validator: {a: 1}, validationAction: "warn"}));

    t.insert({a: 2});
    t.insert({a: 1});
    assert.eq(2, t.count());

})();
