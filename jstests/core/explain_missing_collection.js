// Test explain of various operations against a non-existent collection
(function() {
    var missingColl = db.explain_null_collection;

    var explain;
    var explainColl;

    // .find()
    missingColl.drop();
    explain = missingColl.explain("executionStats").find().finish();
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .count()
    missingColl.drop();
    explain = missingColl.explain("executionStats").count();
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .group()
    missingColl.drop();
    explainColl = missingColl.explain("executionStats");
    explain = explainColl.group({key: "a", initial: {}, reduce: function() {}});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .remove()
    missingColl.drop();
    explain = missingColl.explain("executionStats").remove({a: 1});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .update() with upsert: false
    missingColl.drop();
    explainColl = missingColl.explain("executionStats");
    explain = explainColl.update({a: 1}, {b: 1});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .update() with upsert: true
    missingColl.drop();
    explainColl = missingColl.explain("executionStats");
    explain = explainColl.update({a: 1}, {b: 1}, {upsert: true});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .aggregate()
    missingColl.drop();
    explain = missingColl.explain("executionStats").aggregate([{$match: {a: 1}}]);
    assert.commandWorked(explain);
}());
