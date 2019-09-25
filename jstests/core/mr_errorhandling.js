// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

t = db.mr_errorhandling;
t.drop();

t.save({a: [1, 2, 3]});
t.save({a: [2, 3, 4]});

m_good = function() {
    for (var i = 0; i < this.a.length; i++) {
        emit(this.a[i], 1);
    }
};

m_bad = function() {
    for (var i = 0; i < this.a.length; i++) {
        emit(this.a[i]);
    }
};

r = function(k, v) {
    var total = 0;
    for (var i = 0; i < v.length; i++)
        total += v[i];
    return total;
};

res = t.mapReduce(m_good, r, "mr_errorhandling_out");

assert.eq([{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}],
          db.mr_errorhandling_out.find().sort({_id: 1}).toArray(),
          "A");
db.mr_errorhandling_out.drop();

res = null;

theerror = null;
try {
    res = t.mapReduce(m_bad, r, "mr_errorhandling_out");
} catch (e) {
    theerror = e.toString();
}
assert.isnull(res, "B1");
assert(theerror, "B2");
assert(theerror.indexOf("emit") >= 0, "B3");

// test things are still in an ok state
res = t.mapReduce(m_good, r, "mr_errorhandling_out");
assert.eq([{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}],
          db.mr_errorhandling_out.find().sort({_id: 1}).toArray(),
          "A");
db.mr_errorhandling_out.drop();

assert.throws(function() {
    t.mapReduce(m_good, r, {out: "xxx", query: "foo"});
});

// Test namespace does not exist.
db.mr_nonexistent.drop();
assert.commandFailedWithCode(
    db.runCommand({mapReduce: "mr_nonexistent", map: m_good, reduce: r, out: "out_nonexistent"}),
    ErrorCodes.NamespaceNotFound);

// Test invalid namespace.
assert.commandFailed(
    db.runCommand({mapReduce: /test/, map: m_good, reduce: r, out: "out_nonexistent"}));
