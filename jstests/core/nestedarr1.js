// make sure that we don't crash on large nested arrays but correctly do not index them
// SERVER-5127, SERVER-5036

function makeNestArr(depth) {
    if (depth == 1) {
        return {a: [depth]};
    } else {
        return {a: [makeNestArr(depth - 1)]};
    }
}

t = db.arrNestTest;
t.drop();

t.ensureIndex({a: 1});

n = 1;
while (true) {
    var before = t.count();
    t.insert({_id: n, a: makeNestArr(n)});
    var after = t.count();
    if (before == after)
        break;
    n++;
}

assert(n > 30, "not enough n: " + n);

assert.eq(t.count(), t.find({_id: {$gt: 0}}).hint({a: 1}).itcount());
