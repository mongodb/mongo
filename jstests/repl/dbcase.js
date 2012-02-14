// Test db case checking with replication SERVER-2111

baseName = "jstests_repl_dbcase";

rt = new ReplTest( baseName );

m = rt.start( true );
s = rt.start( false );

n1 = "dbname";
n2 = "dbNAme";

/**
 * The value of n should be n1 or n2.  Check that n is soon present while its
 * opposite is not present.
 */
function check( n ) {
    assert.soon( function() {
                try {
                    // Our db name changes may trigger an exception - SERVER-3189.
                    names = s.getDBNames();
                } catch (e) {
                    return false;
                }
                n1Idx = names.indexOf( n1 );
                n2Idx = names.indexOf( n2 );
                if ( n1Idx != -1 && n2Idx != -1 ) {
                    // n1 and n2 may both be reported as present transiently.
                    return false;
                }
                // Return true if we matched expected n.
                return -1 != names.indexOf( n );
                } );
}

/** Allow some time for additional operations to be processed by the slave. */
function checkTwice( n ) {
 	check( n );
    // zzz is expected to be cloned after n1 and n2 because of its position in the alphabet.
    m.getDB( "zzz" ).c.save( {} );
    assert.soon( function() { return s.getDB( "zzz" ).c.count(); } )
    check( n );
    m.getDB( "zzz" ).dropDatabase();
}

/**
 * The slave may create in memory db names on the master matching old dbs it is
 * attempting to clone.  This function forces operation 'cmd' by deleting those
 * in memory dbs if necessary.  This function should only be called in cases where
 * 'cmd' would succeed if not for the in memory dbs on master created by the slave.
 */
function force( cmd ) {
    print( "cmd: " + cmd );
    eval( cmd );
    while( m1.getLastError() ) {
        sleep( 100 );
    	m1.dropDatabase();
        m2.dropDatabase();
        eval( cmd );
    }
}

m1 = m.getDB( n1 );
m2 = m.getDB( n2 );

m1.c.save( {} );
m2.c.save( {} ); // will fail due to conflict
m1.getLastError(); // Wait for write operations to complete.
check( n1 );

m1.dropDatabase();
force( "m2.c.save( {} );" ); // will now succeed
check( n2 );

m2.dropDatabase();
force( "m1.c.save( {} );" );
check( n1 );

for( i = 0; i < 5; ++i ) {
 	m1.dropDatabase();
    force( "m2.c.save( {} );" );
    m2.dropDatabase();
    force( "m1.c.save( {} );" );
}
checkTwice( n1 );

m1.dropDatabase();
force( "m2.c.save( {} );" );

for( i = 0; i < 5; ++i ) {
    m2.dropDatabase();
    force( "m1.c.save( {} );" );
 	m1.dropDatabase();
    force( "m2.c.save( {} );" );
}
checkTwice( n2 );
