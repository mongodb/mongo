// Tests whether a Replica Set in a mongos cluster can cause versioning problems

jsTestLog( "Starting sharded cluster..." )

var st = new ShardingTest( { shards : 1, mongos : 2, other : { rs : true } } )

var mongosA = st.s0
var mongosB = st.s1
var shard = st.shard0

var sadmin = shard.getDB( "admin" )
sadmin.runCommand({ replSetStepDown : 3000, force : true })

assert.soon( 
    function(){
        var res = sadmin.runCommand( "replSetGetStatus" );
        for ( var i=0; i<res.members.length; i++ ) {
            if ( res.members[i].state == 1 )
                return true;
        }
        return false;
    }
);

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

print( "time to work: " + ( ( end.getTime() - start.getTime() ) / 1000 ) + " seconds" );

assert.gt( 3 , iteratioons );

st.stop()
