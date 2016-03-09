// Test explain of various operations against a non-existent database
(function() {
    var explainMissingDb = db.getSiblingDB("explainMissingDb");

    var explain;
    var explainColl;

    // .find()
    explainMissingDb.dropDatabase();
    explain = explainMissingDb.collection.explain("executionStats").find().finish();
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .count()
    explainMissingDb.dropDatabase();
    explain = explainMissingDb.collection.explain("executionStats").count();
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .group()
    explainMissingDb.dropDatabase();
    explainColl = explainMissingDb.collection.explain("executionStats");
    explain = explainColl.group({key: "a", initial: {}, reduce: function() {}});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .remove()
    explainMissingDb.dropDatabase();
    explain = explainMissingDb.collection.explain("executionStats").remove({a: 1});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .update() with upsert: false
    explainMissingDb.dropDatabase();
    explainColl = explainMissingDb.collection.explain("executionStats");
    explain = explainColl.update({a: 1}, {b: 1});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .update() with upsert: true
    explainMissingDb.dropDatabase();
    explainColl = explainMissingDb.collection.explain("executionStats");
    explain = explainColl.update({a: 1}, {b: 1}, {upsert: true});
    assert.commandWorked(explain);
    assert("executionStats" in explain);

    // .aggregate()
    explainMissingDb.dropDatabase();
    explain = explainMissingDb.collection.explain("executionStats").aggregate([{$match: {a: 1}}]);
    assert.commandWorked(explain);
}());
