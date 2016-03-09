// test cloneCollectionAsCapped

source = db.capped_convertToCapped1;
dest = db.capped_convertToCapped1_clone;

source.drop();
dest.drop();

N = 1000;

for (i = 0; i < N; ++i) {
    source.save({i: i});
}
assert.eq(N, source.count());

// should all fit
res = db.runCommand(
    {cloneCollectionAsCapped: source.getName(), toCollection: dest.getName(), size: 100000});
assert.commandWorked(res);
assert.eq(source.count(), dest.count());
assert.eq(N, source.count());  // didn't delete source

dest.drop();
// should NOT all fit
assert.commandWorked(db.runCommand(
    {cloneCollectionAsCapped: source.getName(), toCollection: dest.getName(), size: 1000}));

assert.eq(N, source.count());  // didn't delete source
assert.gt(source.count(), dest.count());
