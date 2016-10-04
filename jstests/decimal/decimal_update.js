// Test decimal updates

(function() {
    "use strict";
    var col = db.decimal_updates;
    col.drop();

    // Insert some sample data.
    var docs = [
        {'a': NumberDecimal("1.0")},
        {'a': NumberDecimal("0.0")},
        {'a': NumberDecimal("1.00")},
        {'a': NumberLong("1")},
        {'a': 1}
    ];

    assert.writeOK(col.insert(docs), "Initial insertion failed");

    assert.writeOK(col.update({}, {$inc: {'a': NumberDecimal("10")}}, {multi: true}),
                   "update $inc failed");
    assert.eq(col.find({a: 11}).count(), 4, "count after $inc incorrect");
    assert.writeOK(col.update({}, {$inc: {'a': NumberDecimal("0")}}, {multi: true}),
                   "update $inc 0 failed");
    assert.eq(col.find({a: 11}).count(), 4, "count after $inc 0 incorrect");

    col.drop();
    assert.writeOK(col.insert(docs), "Second insertion failed");

    assert.writeOK(col.update({}, {$mul: {'a': NumberDecimal("1")}}, {multi: true}),
                   "update $mul failed");
    assert.eq(col.find({a: 1}).count(), 4, "count after $mul incorrect");
    assert.writeOK(col.update({}, {$mul: {'a': NumberDecimal("2")}}, {multi: true}),
                   "update $mul 2 failed");
    assert.eq(col.find({a: 2}).count(), 4, "count after $mul incorrect");
    assert.writeOK(col.update({}, {$mul: {'a': NumberDecimal("0")}}, {multi: true}),
                   "update $mul 0 failed");
    assert.eq(col.find({a: 0}).count(), 5, "count after $mul 0 incorrect");

    assert.writeError(col.update({}, {$bit: {'a': {and: 1}}}, {multi: true}), "$bit should fail");
}());
