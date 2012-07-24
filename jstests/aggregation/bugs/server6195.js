// ensure $concat asserts on string
c = db.s6570;
c.drop();
c.save({x:17, y:"foo"});

assert.eq(c.aggregate({$project:{_id:0, num : { $concat:[1, "$x", 2, "$x"] }}}).result, [{num:"117217"}] );
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:[3, "$y", 4, "$y"] }}}).result, [{num:"3foo4foo"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:["a", "$x", "b", "$x"] }}}).result, [{num:"a17b17"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:["c", "$y", "d", "$y"] }}}).result, [{num:"cfoodfoo"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:[5, "$y", "e", "$x"] }}}).result, [{num:"5fooe17"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:[6, "$x", "f", "$y"] }}}).result, [{num:"617ffoo"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:["g", "$y", 7, "$x"] }}}).result, [{num:"gfoo717"}]);
assert.eq(c.aggregate({$project:{_id:0, num : { $concat:["h", "$x", 8, "$y"] }}}).result, [{num:"h178foo"}]);
