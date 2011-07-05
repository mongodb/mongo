// csv1.js

t = new ToolTest( "csv1" )

c = t.startDB( "foo" );

base = { a : 1 , b : "foo,bar\"baz,qux" , c: 5, 'd d': -6 , e: '-', f : "."};

assert.eq( 0 , c.count() , "setup1" );
c.insert( base );
delete base._id
assert.eq( 1 , c.count() , "setup2" );

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" , "--csv" , "-f" , "a,b,c,d d,e,f" )

c.drop()
assert.eq( 0 , c.count() , "after drop" )

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" , "--type" , "csv" , "-f" , "a,b,c,d d,e,f" );
assert.soon( "2 == c.count()" , "restore 2" );

a = c.find().sort( { a : 1 } ).toArray();
delete a[0]._id
delete a[1]._id
assert.eq( tojson( { a : "a" , b : "b" , c : "c" , 'd d': "d d", e: 'e', f : "f"} ) , tojson( a[1] ) , "csv parse 1" );
assert.eq( tojson( base ) , tojson(a[0]) , "csv parse 0" )

c.drop()
assert.eq( 0 , c.count() , "after drop 2" )

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo" , "--type" , "csv" , "--headerline" )
assert.soon( "c.findOne()" , "no data after sleep" );
assert.eq( 1 , c.count() , "after restore 2" );

x = c.findOne()
delete x._id;
assert.eq( tojson( base ) , tojson(x) , "csv parse 2" )




t.stop()
