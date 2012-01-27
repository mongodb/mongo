s = new ShardingTest( "bigMapReduce" , 2 , 1 , 1 , { chunksize : 1 } );

// reduce chunk size to split
var config = s.getDB("config");
config.settings.save({_id: "chunksize", value: 1});

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo", key : { "_id" : 1 } } )

db = s.getDB( "test" );
var idInc = 0;
var str=""
for (i=0;i<4*1024;i++) { str=str+"a"; }
for (j=0; j<100; j++) for (i=0; i<512; i++){ db.foo.save({ i : idInc++, y:str})}
db.getLastError();

// Collect some useful stats to figure out what happened
if( db.foo.find().itcount() != 51200 ){
    sleep( 1000 )
    
    s.printShardingStatus(true);
    
    print( "Shard 0: " + s.shard0.getCollection( db.foo + "" ).find().itcount() )
    print( "Shard 1: " + s.shard1.getCollection( db.foo + "" ).find().itcount() )
    
    for( var i = 0; i < 51200; i++ ){
        if( ! db.foo.findOne({ i : i }, { i : 1 }) ){
            print( "Could not find: " + i )
        }
        if( i % 100 == 0 ) print( "Checked " + i )
    }
    
    print( "PROBABLY WILL ASSERT NOW" )
}

assert.soon( function(){ var c = db.foo.find().itcount(); print( "Count is " + c ); return c == 51200 } )
//assert.eq( 51200, db.foo.find().itcount(), "Not all data was saved!" )

s.printChunks();
s.printChangeLog();

function map() { emit('count', 1); } 
function reduce(key, values) { return Array.sum(values) } 

// Test basic mapReduce
for ( iter=0; iter<5; iter++ ){
    out = db.foo.mapReduce(map, reduce,"big_out") 
}

// test output to a different DB
// do it multiple times so that primary shard changes
for (iter = 0; iter < 5; iter++) {
    
    assert.eq( 51200, db.foo.find().itcount(), "Not all data was found!" )
    
    outCollStr = "mr_replace_col_" + iter;
    outDbStr = "mr_db_" + iter;

    print("Testing mr replace into DB " + iter)

    res = db.foo.mapReduce( map , reduce , { out : { replace: outCollStr, db: outDbStr } } )
    printjson(res);

    outDb = s.getDB(outDbStr);
    outColl = outDb[outCollStr];

    obj = outColl.convertToSingleObject("value");
    
    assert.eq( 51200 , obj.count , "Received wrong result " + obj.count );

    print("checking result field");
    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
}

s.stop()

