/**
 * Test 'captrunc' command on indexed capped collections
 */
(function() {
    'use strict';

    db.capped_truncate.drop();
    assert.commandWorked(db.runCommand({ create: "capped_truncate",
                                         capped: true,
                                         size: 1000,
                                         autoIndexId: true }));
    var t = db.capped_truncate;

    for (var j = 1; j <= 10; j++) {
        assert.writeOK(t.insert({x:j}));
    }

    assert.commandWorked(db.runCommand({ captrunc: "capped_truncate", n: 5, inc: false }));
    assert.eq(5, t.count(), "wrong number of documents in capped collection after truncate");
    assert.eq(5, t.distinct("_id").length, "wrong number of entries in _id index after truncate");

    var last = t.find({},{_id:1}).sort({_id:-1}).next();
    assert.neq(null, t.findOne({_id: last._id}),
               tojson(last) + " is in _id index, but not in capped collection after truncate");

})();
