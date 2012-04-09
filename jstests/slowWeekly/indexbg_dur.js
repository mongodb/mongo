/**
 * Kill mongod during a background index build and ensure that the bad index
 * can be dropped on restart.
 */

load( "jstests/libs/slow_weekly_util.js" )

testServer = new SlowWeeklyMongod( "indexbg_dur" )
db = testServer.getDB( "test" );

function countFields( x ) {
    var count = 0;
    for( var i in x ) {
        ++count;
    }
    return count;
}

size = 100000;
while( 1 ) {
    print( "size: " + size );

    var testname = "index_build";
    var path = "/data/db/" + testname+"_dur";
    conn = startMongodEmpty("--port", 30001, "--dbpath", path, "--dur", "--smallfiles", "--durOptions", 8);
    t = conn.getDB( testname ).getCollection( testname );

    for( var i = 0; i < size; ++i ) {
        t.save( {i:i} );
    }
    t.getDB().getLastError();
    x = startMongoProgramNoConnect( "mongo", "--eval", "db.getSisterDB( '" + testname + "' )." + testname + ".ensureIndex( {i:1}, {background:true} );", conn.host );
    sleep( 1000 );
    stopMongod( 30001, /* signal */ 9 );
    waitProgram( x );
    
    conn = startMongodNoReset("--port", 30001, "--dbpath", path, "--dur", "--smallfiles", "--durOptions", 8);
    t = conn.getDB( testname ).getCollection( testname );
    
    var statsSize = countFields( t.stats().indexSizes );
    var nsSize = conn.getDB( testname ).system.indexes.count( {ns:testname+'.'+testname} );
    
    // If index build completed before the kill, try again with more data.
    if ( !( statsSize == 1 && nsSize == 2 ) ) {
        print( "statsSize: " + statsSize + ", nsSize: " + nsSize + ", retrying with more data" );
        stopMongod( 30001 );
        size *= 2;
        continue;
    }
    
    assert.eq( "index not found", t.dropIndex( "i_1" ).errmsg );
    
    var statsSize = countFields( t.stats().indexSizes );
    var nsSize = conn.getDB( testname ).system.indexes.count( {ns:testname+'.'+testname} );

    assert.eq( statsSize, nsSize );
    assert( t.validate().valid );
    // TODO check that index namespace is cleaned up as well once that is implemented
    
    t.ensureIndex( {i:1} );
    var statsSize = countFields( t.stats().indexSizes );
    var nsSize = conn.getDB( testname ).system.indexes.count( {ns:testname+'.'+testname} );

    assert.eq( 2, statsSize );
    assert.eq( 2, nsSize );
    
    exp = t.find( {i:20} ).explain();
    assert.eq( 1, exp.n );
    assert.eq( 'BtreeCursor i_1', exp.cursor );
    
    break;
}   

testServer.stop();
