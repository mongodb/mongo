
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
assert.eq({1: 1, 2: 2, 3: 2, 4: 1}, res.convertToSingleObject(), "A");
res.drop();

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
assert.eq({1: 1, 2: 2, 3: 2, 4: 1}, res.convertToSingleObject(), "A");
res.drop();

assert.throws(function() {
    t.mapReduce(m_good, r, {out: "xxx", query: "foo"});
});
