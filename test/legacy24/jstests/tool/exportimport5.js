// exportimport4.js

t = new ToolTest( "exportimport5" );
c = t.startDB( "foo" );

install_test_data = function() {
    c.drop();

    assert.eq( 0 , c.count() , "setup1" );

    c.save( { a : [1, 2, 3, Infinity, 4, null, 5] } );
    c.save( { a : [1, 2, 3, 4, 5] } );
    c.save( { a : [ Infinity ] } );
    c.save( { a : [1, 2, 3, 4, Infinity, Infinity, 5, -Infinity] } );
    c.save( { a : [1, 2, 3, 4, null, null, 5, null] } );
    c.save( { a : [ -Infinity ] } );

    assert.eq( 6 , c.count() , "setup2" );
};

// attempt to export fields without Infinity
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: { \"$nin\": [ Infinity ] } }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 3 , c.count() , "after restore 1" );

// attempt to export fields with Infinity
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: Infinity }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 3 , c.count() , "after restore 2" );

// attempt to export fields without -Infinity
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: { \"$nin\": [ -Infinity ] } }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 4 , c.count() , "after restore 3" );

// attempt to export fields with -Infinity
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: -Infinity }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 2 , c.count() , "after restore 4" );

// attempt to export everything
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 6 , c.count() , "after restore 5" );

t.stop();
