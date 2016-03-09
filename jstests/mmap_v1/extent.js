t = db.reclaimExtentsTest;
t.drop();

for (var i = 0; i < 50; i++) {  // enough iterations to break 32 bit.
    db.createCollection('reclaimExtentsTest', {size: 100000000});
    t.insert({x: 1});
    assert(t.count() == 1);
    t.drop();
}
t.drop();
