
t = db.mr_index;
t.drop();

outName = "mr_index_out";
out = db[outName];
out.drop();

t.insert({tags: [1]});
t.insert({tags: [1, 2]});
t.insert({tags: [1, 2, 3]});
t.insert({tags: [3]});
t.insert({tags: [2, 3]});
t.insert({tags: [2, 3]});
t.insert({tags: [1, 2]});

m = function() {
    for (i = 0; i < this.tags.length; i++)
        emit(this.tags[i], 1);
};

r = function(k, vs) {
    return Array.sum(vs);
};

ex = function() {
    return out.find().sort({value: 1}).explain("executionStats");
};

res = t.mapReduce(m, r, {out: outName});

assert.eq(3, ex().executionStats.nReturned, "A1");
out.ensureIndex({value: 1});
assert.eq(3, ex().executionStats.nReturned, "A2");

res = t.mapReduce(m, r, {out: outName});

assert.eq(3, ex().executionStats.nReturned, "B1");
res.drop();
