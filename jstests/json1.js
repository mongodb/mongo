
x = { quotes:"a\"b" , nulls:null };
eval( "y = " + tojson( x ) );
assert.eq( tojson( x ) , tojson( y ) , "A" );
assert.eq( typeof( x.nulls ) , typeof( y.nulls ) , "B" );

// each type is parsed properly
x = {"x" : null, "y" : true, "z" : 123, "w" : "foo", "a": undefined};
assert.eq(tojson(x,"",false), '{\n\t"x" : null,\n\t"y" : true,\n\t"z" : 123,\n\t"w" : "foo",\n\t"a" : undefined\n}' , "C" );

x = {"x" : [], "y" : {}};
assert.eq(tojson(x,"",false), '{\n\t"x" : [ ],\n\t"y" : {\n\t\t\n\t}\n}' , "D" );

// nested
x = {"x" : [{"x" : [1,2,[]], "z" : "ok", "y" : [[]]}, {"foo" : "bar"}], "y" : null};
assert.eq(tojson(x), '{\n\t"x" : [\n\t\t{\n\t\t\t"x" : [\n\t\t\t\t1,\n\t\t\t\t2,\n\t\t\t\t[ ]\n\t\t\t],\n\t\t\t"z" : "ok",\n\t\t\t"y" : [\n\t\t\t\t[ ]\n\t\t\t]\n\t\t},\n\t\t{\n\t\t\t"foo" : "bar"\n\t\t}\n\t],\n\t"y" : null\n}' , "E" );

// special types
x = {"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),  'z' : /xd?/ig};
assert.eq(tojson(x,"",false), '{\n\t"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),\n\t"z" : /xd?/gi\n}' , "F" );

// Timestamp type
x = {"x" : Timestamp()};
assert.eq(tojson(x,"",false), '{\n\t"x" : Timestamp(0, 0)\n}' , "G")

// Timestamp type, second
x = {"x" : Timestamp(10,2)};
assert.eq(tojson(x,"",false), '{\n\t"x" : Timestamp(10, 2)\n}' , "H")
