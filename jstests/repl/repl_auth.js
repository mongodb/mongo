// Test repl with auth enabled

var baseName = "jstests_repl11test";
var keyFilePath = "jstests/libs/key1";

setAdmin = function( n ) {
    n.getDB( "admin" ).addUser( "super", "super", jsTest.adminUserRoles, 3 );
}

auth = function( n ) {
    return n.getDB( baseName ).auth( "test", "test" );
}

doTest = function(signal, extraOpts) {

    rt = new ReplTest( baseName );
    
    m = rt.start( true, {}, false, true );
    m.getDB( baseName ).addUser( "test", "test", jsTest.basicUserRoles, 3 );

    setAdmin( m );
    rt.stop( true );
    
    s = rt.start( false, {}, false, true );
    rt.stop( false );
    
    m = rt.start( true, { auth:null, keyFile: keyFilePath }, true );
    auth( m );

    var slaveOpt = { auth: null, keyFile: keyFilePath };
    slaveOpt = Object.extend(slaveOpt, extraOpts);

    s = rt.start( false, slaveOpt, true );
    assert.soon( function() { return auth( s ); } );
    
    ma = m.getDB( baseName ).a;    
    ma.save( {} );
    sa = s.getDB( baseName ).a;
    assert.soon( function() { return 1 == sa.count(); } );
    
    s.getDB( "admin" ).auth( "super", "super" );
    assert.commandWorked( s.getDB( "admin" )._adminCommand( {serverStatus:1,repl:1} ) );
    assert.commandWorked( s.getDB( "admin" )._adminCommand( {serverStatus:1,repl:2} ) );
    
    rt.stop( false, signal );
    
    ma.save( {} );
    s = rt.start(false, slaveOpt, true);
    assert.soon( function() { return auth( s ); } );
    sa = s.getDB( baseName ).a;
    assert.soon( function() { return 2 == sa.count(); } );
    
    ma.save( {a:1} );
    assert.soon( function() { return 1 == sa.count( {a:1} ); } );
    
    ma.update( {a:1}, {b:2} );
    assert.soon( function() { return 1 == sa.count( {b:2} ); } );
    
    ma.remove( {b:2} );
    assert.soon( function() { return 0 == sa.count( {b:2} ); } );
    
    rt.stop();
}

doTest( 15 ); // SIGTERM
doTest(9, { journal: null });  // SIGKILL
