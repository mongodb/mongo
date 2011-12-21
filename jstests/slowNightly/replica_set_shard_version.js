// Tests whether a Replica Set in a mongos cluster can cause versioning problems

jsTestLog( "Starting sharded cluster..." )

var st = new ShardingTest( { shards : 1, mongos : 2, other : { rs : true } } )

var mongosA = st.s0
var mongosB = st.s1
var rs = st._rs[0].test
var shard = st.shard0

var sadmin = shard.getDB( "admin" )

jsTestLog( "Stepping down replica set member..." )

try{
    sadmin.runCommand({ replSetStepDown : 3000, force : true })
}
catch( e ){
    // stepdown errors out our conn to the shard
    printjson( e )
}

jsTestLog( "Reconnecting..." )

sadmin = new Mongo( st.shard0.host ).getDB("admin")

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

jsTestLog( "New primary elected..." )

coll = mongosA.getCollection( jsTestName() + ".coll" );

start = new Date();

ReplSetTest.awaitRSClientHosts( coll.getMongo(), rs.getPrimary(), { ismaster : true }, rs )

try{
    coll.findOne()
}
catch( e ){
    printjson( e )
    assert( false )
}

end = new Date();

print( "time to work for primary: " + ( ( end.getTime() - start.getTime() ) / 1000 ) + " seconds" );

jsTestLog( "Found data from collection..." )

// now check secondary

try{
    sadmin.runCommand({ replSetStepDown : 3000, force : true })
}
catch( e ){
    // expected, since all conns closed
    printjson( e )
}

sadmin = new Mongo( st.shard0.host ).getDB("admin")

jsTestLog( "Stepped down secondary..." )

other = new Mongo( mongosA.host );
other.setSlaveOk( true );
other = other.getCollection( jsTestName() + ".coll" );

print( "eliot: " + tojson( other.findOne() ) );



st.stop()
