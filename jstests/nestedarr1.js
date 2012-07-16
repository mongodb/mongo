//SERVER-5127, SERVER-5036

function makeNestArr(depth){
	if(depth == 1){
		return {a : [depth]};
	}
	else{
		return {a : [makeNestArr(depth - 1)] };
	}
}

t = db.arrNestTest;
t.drop();
t.ensureIndex({a:1});

nestedArr = makeNestArr(150);

t.save( { tst : "test1", a : nestedArr } );
t.save( { tst : "test2", a : nestedArr } );
t.save( { tst : "test3", a : nestedArr } );

assert.eq(3, t.count(), "records in collection");
assert.eq(1, t.find({tst : "test2"}).count(), "find test");

//make sure index insertion failed (nesting must be large enough)
assert.eq(0, t.find().hint({a:1}).explain().n, "index not empty");
print("Test succeeded!")
