/* SERVER-6076 support for running a single command
 * This tests that setting loopCommands boolean to false won't make the threads
 * loop over the ops array. This will ensure that the operations in the ops array 
 * are executed by a thread only once.
 */

var coll = db.bench_test4;
coll.drop();

for ( var i = 0; i < 10; i++ ) {
    coll.insert( { a : 1 } )
    db.getLastError();
}

ops =  [ { op : "find", ns : coll.getFullName(), query : { a : 1 } },
         { op : "insert", doc : { x : 2 },  ns : coll.getFullName(), safe : true } ]

var res = benchRun( { parallel : 4, seconds : 1, ops : ops, loopCommands : false,
                      host : db.getMongo().host } );

// make sure that exactly 4 query/insert operations/second were performed
// each thread would have performed the find/insert operation exactly once
assert.eq( 4, res.query , "loopCommand failed" );
assert.eq( 4, res.insert , "loopCommand failed" );
