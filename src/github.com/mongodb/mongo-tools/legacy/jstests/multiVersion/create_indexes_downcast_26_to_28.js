// test downcasting createIndexes command to insert 2.6 -> 2.4

load( './jstests/multiVersion/libs/multi_rs.js' );
load( './jstests/multiVersion/libs/multi_cluster.js' );

var oldVersion = "2.6";
var newVersion = "latest";

var options = {

    mongosOptions : { binVersion : newVersion },
    configOptions : { binVersion : oldVersion },
    shardOptions : { binVersion : oldVersion },

    separateConfig : true,
    sync : false,
    rs : false
};

var st = new ShardingTest({ shards : 1, mongos : 1, other : options });

var mydb = st.s.getDB( "test" );

mydb.foo.insert( { x : 1 } );

assert.eq( 1, mydb.foo.getIndexes().length );
res = mydb.foo.runCommand( "createIndexes", { indexes : [ { key : { "x" : 1 }, name : "x_1" } ] } );
printjson( res );
assert.eq( 2, mydb.foo.getIndexes().length );
printjson( mydb.foo.getIndexes() );

assert.eq( 2, mydb.foo.getIndexes().length );
res = mydb.foo.runCommand( "createIndexes", { indexes : [ { key : { "a" : 1 }, name : "a_1" },
                                                          { key : { "b" : 1 }, name : "b_1" } ] } );
printjson( res );
assert.eq( 4, mydb.foo.getIndexes().length );
printjson( mydb.foo.getIndexes() );

st.stop();


