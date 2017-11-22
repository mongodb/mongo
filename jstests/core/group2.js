// Cannot implicitly shard accessed collections because of unsupported group operator on sharded
// collection.
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";
    const coll = db.group2;
    coll.drop();

    assert.writeOK(coll.insert({a: 2}));
    assert.writeOK(coll.insert({b: 5}));
    assert.writeOK(coll.insert({a: 1}));

    const cmd = {
        key: {a: 1},
        initial: {count: 0},
        reduce: function(obj, prev) {
            prev.count++;
        }
    };
    const sortFunc = function(doc1, doc2) {
        if (doc1.a < doc2.a) {
            return -1;
        } else if (doc1.a > doc2.a) {
            return 1;
        } else {
            return 0;
        }
    };
    const expected = [{a: null, count: 1}, {a: 1, count: 1}, {a: 2, count: 1}];
    assert.eq(coll.group(cmd).sort(sortFunc), expected);

    const keyFn = function(x) {
        return {a: 'a' in x ? x.a : null};
    };

    delete cmd.key;
    cmd["$keyf"] = keyFn;
    assert.eq(coll.group(cmd).sort(sortFunc), expected);

    delete cmd.$keyf;
    cmd["keyf"] = keyFn;
    assert.eq(coll.group(cmd).sort(sortFunc), expected);
}());
