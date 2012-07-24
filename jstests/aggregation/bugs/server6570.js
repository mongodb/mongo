// ensure $add asserts on string
c = db.s6570;
c.drop();
c.save({x:17, y:"foo"});

assert.eq(c.aggregate({$project:{string_fields : { $add:[3, "$y", 4, "$y"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{number_fields : { $add:["a", "$x", "b", "$x"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{all_strings : { $add:["c", "$y", "d", "$y"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{potpourri_1 : { $add:[5, "$y", "e", "$x"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{potpourri_2 : { $add:[6, "$x", "f", "$y"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{potpourri_3 : { $add:["g", "$y", 7, "$x"] }}}).code, 16416);
assert.eq(c.aggregate({$project:{potpourri_4 : { $add:["h", "$x", 8, "$y"] }}}).code, 16416);
