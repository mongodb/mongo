// Cannot implicitly shard accessed collections because of unsupported group operator on sharded
// collection.
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    const coll = db.group1;
    coll.drop();

    assert.writeOK(coll.insert({n: 1, a: 1}));
    assert.writeOK(coll.insert({n: 2, a: 1}));
    assert.writeOK(coll.insert({n: 3, a: 2}));
    assert.writeOK(coll.insert({n: 4, a: 2}));
    assert.writeOK(coll.insert({n: 5, a: 2}));

    let p = {
        key: {a: true},
        reduce: function(obj, prev) {
            prev.count++;
        },
        initial: {count: 0}
    };

    function sortFuncGenerator(key) {
        return (doc1, doc2) => {
            if (doc1[key] < doc2[key]) {
                return -1;
            } else if (doc1[key] > doc2[key]) {
                return 1;
            } else {
                return 0;
            }
        };
    }

    const sortOnA = sortFuncGenerator("a");
    let expected = [{a: 1, count: 2}, {a: 2, count: 3}];
    let result = coll.group(p).sort(sortOnA);
    assert.eq(result, expected);

    result = coll.groupcmd(p).sort(sortOnA);
    assert.eq(result, expected);

    expected = [{count: 5}];
    result = coll.groupcmd({key: {}, reduce: p.reduce, initial: p.initial});
    assert.eq(result, expected);

    expected = [{sum: 15}];
    result = coll.groupcmd({
        key: {},
        reduce: function(obj, prev) {
            prev.sum += obj.n;
        },
        initial: {sum: 0}
    });
    assert.eq(result, expected);

    assert(coll.drop());

    assert.writeOK(coll.insert({"a": 2}));
    assert.writeOK(coll.insert({"b": 5}));
    assert.writeOK(coll.insert({"a": 1}));
    assert.writeOK(coll.insert({"a": 2}));

    const c = {
        key: {a: 1},
        cond: {},
        initial: {"count": 0},
        reduce: function(obj, prev) {
            prev.count++;
        }
    };

    expected = [{a: null, count: 1}, {a: 1, count: 1}, {a: 2, count: 2}];
    assert.eq(coll.group(c).sort(sortOnA), expected);
    assert.eq(coll.groupcmd(c).sort(sortOnA), expected);

    assert(coll.drop());

    assert.writeOK(coll.insert({name: {first: "a", last: "A"}}));
    assert.writeOK(coll.insert({name: {first: "b", last: "B"}}));
    assert.writeOK(coll.insert({name: {first: "a", last: "A"}}));

    p = {
        key: {'name.first': true},
        reduce: function(obj, prev) {
            prev.count++;
        },
        initial: {count: 0}
    };
    const sortOnNameDotFirst = sortFuncGenerator("name.first");

    expected = [{"name.first": "a", count: 2}, {"name.first": "b", count: 1}];
    assert.eq(coll.group(p).sort(sortOnNameDotFirst), expected);

    // SERVER-15851 Test invalid user input.
    p = {
        ns: "group1",
        key: {"name.first": true},
        $reduce: function(obj, prev) {
            prev.count++;
        },
        initial: {count: 0},
        finalize: "abc"
    };
    assert.commandFailedWithCode(
        db.runCommand({group: p}), ErrorCodes.JSInterpreterFailure, "Illegal finalize function");

    p = {
        ns: "group1",
        key: {"name.first": true},
        $reduce: function(obj, prev) {
            prev.count++;
        },
        initial: {count: 0},
        finalize: function(obj) {
            throw new Error("Intentionally throwing exception in finalize function");
        }
    };
    assert.commandFailedWithCode(
        db.runCommand({group: p}), ErrorCodes.JSInterpreterFailure, "Illegal finalize function 2");

    p = {
        ns: "group1",
        $keyf: "a",
        $reduce: function(obj, prev) {
            prev.count++;
        },
        initial: {count: 0},
        finalize: function(obj) {
            throw new Error("Intentionally throwing exception in finalize function");
        }
    };
    assert.commandFailedWithCode(
        db.runCommand({group: p}), ErrorCodes.JSInterpreterFailure, "Illegal keyf function");

    p = {ns: "group1", key: {"name.first": true}, $reduce: "abc", initial: {count: 0}};
    assert.commandFailedWithCode(
        db.runCommand({group: p}), ErrorCodes.JSInterpreterFailure, "Illegal reduce function");

    p = {
        ns: "group1",
        key: {"name.first": true},
        $reduce: function(obj, pre) {
            prev.count++;
        },
        initial: {count: 0}
    };
    assert.commandFailedWithCode(
        db.runCommand({group: p}), ErrorCodes.JSInterpreterFailure, "Illegal reduce function 2");
}());
