// SERVER-8514

var t = db.server8514;

t.drop();

var query_good = {a: {$in: [null]}};
var query_bad = {a: {$in: null}};

function  getCount(q) {
    try {
        return t.find(q).count();
    }
    catch (e) {
      return e;
    }
}

result = getCount(query_good);
assert.eq(0, result);
result = getCount(query_bad);
assert(result.message.match(/count failed/) !== null);

t.save({a: [1, 2]});

// query on non-empty collection should yield zero documents
// since the predicate is invalid
result = getCount(query_good);
assert.eq(0, result);
result = getCount(query_bad);
assert(result.message.match(/count failed/) !== null);