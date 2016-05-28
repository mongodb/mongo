// SERVER-5127, SERVER-5036

function makeNestObj(depth) {
    toret = {a: 1};

    for (i = 1; i < depth; i++) {
        toret = {a: toret};
    }

    return toret;
}

t = db.objNestTest;
t.drop();

t.ensureIndex({a: 1});

n = 1;
while (true) {
    var before = t.count();
    t.insert({_id: n, a: makeNestObj(n)});
    var after = t.count();
    if (before == after)
        break;
    n++;
}

assert(n > 30, "not enough n: " + n);

assert.eq(t.count(), t.find({_id: {$gt: 0}}).hint({a: 1}).itcount());
