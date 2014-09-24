// exportimport4.js

t = new ToolTest( "exportimport4" );
c = t.startDB( "foo" );

install_test_data = function() {
    c.drop();

    assert.eq( 0 , c.count() , "setup1" );

    c.save( { a : [1, 2, 3, NaN, 4, null, 5] } );
    c.save( { a : [1, 2, 3, 4, 5] } );
    c.save( { a : [ NaN ] } );
    c.save( { a : [1, 2, 3, 4, NaN, NaN, 5, NaN] } );
    c.save( { a : [1, 2, 3, 4, null, null, 5, null] } );

    assert.eq( 5 , c.count() , "setup2" );
};

// attempt to export fields without NaN
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: { \"$nin\": [ NaN ] } }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 2 , c.count() , "after restore 1" );

// attempt to export fields with NaN
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo", "-q", "{ a: NaN }" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 3 , c.count() , "after restore 2" );

// attempt to export everything
install_test_data();

t.runTool( "export" , "--out" , t.extFile , "-d" , t.baseName , "-c" , "foo" );

c.drop();
assert.eq( 0 , c.count() , "after drop" , "-d" , t.baseName , "-c" , "foo" );

t.runTool( "import" , "--file" , t.extFile , "-d" , t.baseName , "-c" , "foo", "--drop" );

assert.eq( 5 , c.count() , "after restore 3" );

t.stop();
