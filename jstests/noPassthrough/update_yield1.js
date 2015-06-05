load( "jstests/libs/slow_weekly_util.js" );
var testServer = new SlowWeeklyMongod( "update_yield1" );
var db = testServer.getDB( "test" );
testServer.getDB("admin").runCommand( {setParameter:1, ttlMonitorEnabled : false} );

var t = db.update_yield1;
t.drop();

var N = 640000;
var i = 0;

while ( true ){
    var fill = function() {
        var bulk = t.initializeUnorderedBulkOp();
        for ( ; i<N; i++ ){
            bulk.insert({ _id: i, n: 1 });
        }
        assert.writeOK(bulk.execute());
    };

    var timeUpdate = function() {
        return Date.timeFunc(
            function(){
                t.update( {} , { $inc : { n : 1 } } , false , true );
            }
        );
    };

    fill();
    timeUpdate();
    timeUpdate();
    var time = timeUpdate();
    print( N + "\t" + time );
    if ( time > 8000 )
        break;

    N *= 2;
}

function haveInProgressUpdate() {
    var ops = db.currentOp();
    printjson(ops);
    return ops.inprog.some(
        function(elt) {
            return elt.op == "update";
        });
}

// --- test 1

var join = startParallelShell( "db.update_yield1.update( {}, { $inc: { n: 1 }}, false, true );" );
assert.soon(haveInProgressUpdate, "never doing update");

var num = 0;
var start = new Date();
while ( ( (new Date()).getTime() - start ) < ( time * 2 ) ){
    var me = Date.timeFunc( function(){ t.findOne(); } );
    if (me > 50) print("time: " + me);

    if ( num++ == 0 ){
        var x = db.currentOp();
        // one operation for the update, another for currentOp itself.  There may be other internal
        // operations running.
        assert.gte( 2 , x.inprog.length , "nothing in prog" );
    }

    assert.gt( time / 3 , me );
}

join();

x = db.currentOp();
// currentOp itself shows up as an active operation
assert.eq( 1 , x.inprog.length , "weird 2" );

testServer.stop();
