// SERVER-8514

var t = db.server8514;

t.drop();

var q = {a: {$in: [null]}};

function  getCount() {
    try {
        return t.find(q).count();
    }
    catch (e) {
      return e;
    }
}

result = getCount();
assert(result.message.match(/count failed/) !== null);
assert(result.message.match(/26/) !== null);

t.save({a: [1, 2]});

// query on non-empty collection should yield zero documents
// since the predicate is invalid
result = getCount();
assert.eq(0, result);