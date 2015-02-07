/**
 * Test that a capped collection ttl index doesn't cause the server to shut down, 
 * nor stop processing collections in the db
 * TODO: Change this test to show that you can't create capped collection ttl index 
 *       Will need to figure out how to test that failed index ttl processing doesn't block others
 */
(function() {
    "use strict";
    var baseDir = "jstests_ttl_capped";
    var port = allocatePorts( 1 )[ 0 ];
    var dbpath = MongoRunner.dataPath + baseDir + "/";
    
    var m = MongoRunner.runMongod({
                                    dbpath: dbpath,
                                    port: port,
                                    setParameter:"ttlMonitorSleepSecs=1"});
    var db = m.getDB( "test" );
    
    // Make sure we have collections before and after the capped one, so we can check they work.
    var bc = db.ttl_before;
    var t = db.ttl_capped;
    var ac = db.ttl_zafter;
    
    t.drop();
    bc.drop();
    ac.drop();
    var dt = (new Date()).getTime();
    jsTest.log("using date on inserted docs: " + tojson(new Date(dt)));
    
    // increase logging
    assert.commandWorked(db.adminCommand({setParameter:1, logLevel:1}));

    bc.ensureIndex( { x : 1 } , { expireAfterSeconds : -1 } );
    assert.commandWorked(t.runCommand( "create", { capped:true, size:100, maxCount:100} ));
    t.ensureIndex( { x : 1 } , { expireAfterSeconds : -1 } );
    ac.ensureIndex( { x : 1 } , { expireAfterSeconds : -1 } );
    
    assert.writeOK(bc.insert({x: new Date(dt)}));
    assert.writeOK(t.insert({x: new Date(dt)}));
    assert.writeOK(ac.insert({x: new Date(dt)}));
    
    assert.eq(bc.count(), 1);
    assert.eq(t.count(), 1);
    assert.eq(ac.count(), 1);

    sleep(1.1 * 1000); // 2 second sleep
    jsTest.log("TTL work should be done.")
    assert.eq(bc.count(), 0);
    assert.eq(t.count(), 1);
    assert.eq(ac.count(), 0);
    
    MongoRunner.stopMongod(port);
})();