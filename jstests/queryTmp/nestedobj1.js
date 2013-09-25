//SERVER-5127, SERVER-5036

function makeNestObj(depth){
    toret = { a : 1};

    for(i = 1; i < depth; i++){
        toret = {a : toret};
    }

    return toret;
}

t = db.objNestTest;
t.drop();
t.ensureIndex({a:1});

nestedObj = makeNestObj(300);

t.insert( { tst : "test1", a : nestedObj }, true );
t.insert( { tst : "test2", a : nestedObj }, true );
t.insert( { tst : "test3", a : nestedObj }, true );

assert.eq(3, t.count(), "records in collection");
assert.eq(1, t.find({tst : "test2"}).count(), "find test");

//make sure index insertion failed (nesting must be large enough)
assert.eq(0, t.find().hint({a:1}).explain().n, "index not empty");
print("Test succeeded!")
