/** Test running out of disk space with durability enabled */

startPath = "/data/db/diskfulltest";
recoverPath = "/data/db/dur_diskfull";

doIt = false;
files = listFiles( "/data/db" );
for ( i in files ) {
    if ( files[ i ].name == startPath ) {
        doIt = true;
    }
}

if ( !doIt ) {
    print( "path " + startPath + " missing, skipping diskfull test" );
    doIt = false;
}

function checkNoJournalFiles(path, pass) {
    var files = listFiles(path);
    if (files.some(function (f) { return f.name.indexOf("prealloc") < 0; })) {
        if (pass == null) {
            // wait a bit longer for mongod to potentially finish if it is still running.
            sleep(10000);
            return checkNoJournalFiles(path, 1);
        }
        print("\n\n\n");
        print("FAIL path:" + path);
        print("unexpected files:");
        printjson(files);
        assert(false, "FAIL a journal/lsn file is present which is unexpected");
    }
}

/** Clear dbpath without removing and recreating diskfulltest directory, as resetDbpath does */
function clear() {
    files = listFiles( startPath );
    files.forEach( function( x ) { removeFile( x.name ) } );
}

function log(str) {
    print();
    if(str)
        print(testname+" step " + step++ + " " + str);
    else
        print(testname+" step " + step++);
}

function work() {
    log("work");
    try {
        var d = conn.getDB("test");
        
        big = new Array( 5000 ).toString();
        for( i = 0; i < 10000; ++i ) {
            d.foo.insert( { _id:i, b:big } );
        }
        
        gle = d.getLastError();
        if ( gle )
            throw gle;
    } catch ( e ) {
        print( e );
        raise( e );
    } finally {
        log("endwork");
    }
}

function verify() { 
    log("verify");
    var d = conn.getDB("test");
    c = d.foo.count();
    v = d.foo.validate();
    // not much we can guarantee about the writes, just validate when possible
    if ( c != 0 && !v.valid ) {
        printjson( v );
        print( c );
        assert( v.valid );
        assert.gt( c, 0 );
    }
}

function runFirstMongodAndFillDisk() {
    log();
    
    clear();
    conn = startMongodNoReset("--port", 30001, "--dbpath", startPath, "--dur", "--smallfiles", "--durOptions", 8+64, "--noprealloc");
    
    assert.throws( work, null, "no exception thrown when exceeding disk capacity" );
    stopMongod( 30001 );

    sleep(5000);    
}

function runSecondMongdAndRecover() {
    // restart and recover
    log();
    conn = startMongodNoReset("--port", 30003, "--dbpath", startPath, "--dur", "--smallfiles", "--durOptions", 8+64, "--noprealloc");
    verify();
    
    log("stop");
    stopMongod(30003);
    
    // stopMongod seems to be asynchronous (hmmm) so we sleep here.
    sleep(5000);
    
    // at this point, after clean shutdown, there should be no journal files
    log("check no journal files");
    checkNoJournalFiles(startPath + "/journal/");
    
    log();    
}

function someWritesInJournal() {
    runFirstMongodAndFillDisk();
    runSecondMongdAndRecover();
}

function noWritesInJournal() {
    // It is too difficult to consistently trigger cases where there are no existing journal files due to lack of disk space, but
    // if we were to test this case we would need to manualy remove the lock file.
//    removeFile( startPath + "/mongod.lock" );
}

if ( doIt ) {

    var testname = "dur_diskfull";
    var step = 1;
    var conn = null;
 
    someWritesInJournal();
    noWritesInJournal();
    
    print(testname + " SUCCESS");    

}
