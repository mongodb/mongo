// Tests whether a Replica Set in a mongos cluster can cause versioning problems

jsTestLog( "Starting sharded cluster..." )

var st = new ShardingTest( { shards : 1, mongos : 2, other : { rs : true } } )

var mongosA = st.s0
var mongosB = st.s1
var shard = st.shard0

var sadmin = shard.getDB( "admin" )
assert.throws(function() { sadmin.runCommand({ replSetStepDown : 3000, force : true }); });
try { sadmin.getLastError(); } catch (e) { print("reconnecting: "+e); }

st.rs0.getMaster();

mongosA.getDB("admin").runCommand({ setParameter : 1, traceExceptions : true })

coll = mongosA.getCollection( jsTestName() + ".coll" );

start = new Date();

iteratioons = 0;

assert.soon(
    function(z){
        iteratioons++;
        try {
            coll.findOne();
            return true;
        }
        catch ( e ){
            return false;
        }
    } );

printjson( coll.findOne() )

end = new Date();

mongosA.getDB("admin").runCommand({ setParameter : 1, traceExceptions : false })

print( "time to work for primary: " + ( ( end.getTime() - start.getTime() ) / 1000 ) + " seconds" );

assert.gt( 3 , iteratioons );


// now check secondary

assert.throws(function() { sadmin.runCommand({ replSetStepDown : 3000, force : true }); });
try { sadmin.getLastError(); } catch (e) { print("reconnecting: "+e); }

other = new Mongo( mongosA.host );
other.setSlaveOk( true );
other = other.getCollection( jsTestName() + ".coll" );

print( "eliot: " + tojson( other.findOne() ) );



st.stop()
