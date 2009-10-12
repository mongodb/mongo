
x = { quotes:"a\"b" , nulls:null };
eval( "y = " + tojson( x ) );
assert.eq( tojson( x ) , tojson( y ) );
assert.eq( typeof( x.nulls ) , typeof( y.nulls ) );

// each type is parsed properly
x = {"x" : null, "y" : true, "z" : 123, "w" : "foo"};
assert.eq(tojson(x), '{\n\t"x" : null,\n\t"y" : true,\n\t"z" : 123,\n\t"w" : "foo"\n}');

x = {"x" : [], "y" : {}};
assert.eq(tojson(x), '{\n\t"x" : [\n\t\t\n\t],\n\t"y" : {\n\t\t\n\t}\n}');

// nested
x = {"x" : [{"x" : [1,2,[]], "4" : "ok", "y" : [[]]}, {"foo" : "bar"}], "y" : null};
assert.eq(tojson(x), '{\n\t"x" : [\n\t\t{\n\t\t\t"x" : [\n\t\t\t\t1,\n\t\t\t\t2,\n\t\t\t\t[\n\t\t\t\t\t\n\t\t\t\t]\n\t\t\t],\n\t\t\t"4" : "ok",\n\t\t\t"y" : [\n\t\t\t\t[\n\t\t\t\t\t\n\t\t\t\t]\n\t\t\t]\n\t\t},\n\t\t{\n\t\t\t"foo" : "bar"\n\t\t}\n\t],\n\t"y" : null\n}');

// special types
x = {"x" : ObjectId("4ad35a73d2e34eb4fc43579a"), 'y' : new Date("10/12/1998"),  'z' : /xd?/ig};
assert.eq(tojson(x), '{\n\t"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),\n\t"y" : "Mon Oct 12 1998 00:00:00 GMT-0400 (EDT)",\n\t"z" : /xd?/gi\n}');
