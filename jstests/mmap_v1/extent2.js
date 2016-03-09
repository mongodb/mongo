

mydb = db.getSisterDB("test_extent2");
mydb.dropDatabase();

t = mydb.foo;

function insert() {
    t.insert({_id: 1, x: 1});
    t.insert({_id: 2, x: 1});
    t.insert({_id: 3, x: 1});
    t.ensureIndex({x: 1});
}

insert();
t.drop();

start = mydb.stats();

for (i = 0; i < 100; i++) {
    insert();
    t.drop();
}

end = mydb.stats();

printjson(start);
printjson(end);
assert.eq(start.extentFreeList.num, end.extentFreeList.num);

// 3: 1 data, 1 _id idx, 1 x idx
// used to be 4, but we no longer waste an extent for the freelist
assert.eq(3, start.extentFreeList.num);
assert.eq(3, end.extentFreeList.num);
