s = new ShardingTest( "bigMapReduce" , 2 , 1 , 1 , { chunksize : 1 } );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo", key : { "_id" : 1 } } )

db = s.getDB( "test" );
var str=""
for (i=0;i<4*1024;i++) { str=str+"a"; }
for (j=0; j<50; j++) for (i=0; i<512; i++){ db.foo.save({y:str})}
db.getLastError();

s.printChunks();
s.printChangeLog();

function map() { emit('count', 1); } 
function reduce(key, values) { return Array.sum(values) } 

out = db.foo.mapReduce(map, reduce,"big_out") 
printjson(out) // SERVER-1400

// test output to a different DB
// do it multiple times so that primary shard changes
for (iter = 0; iter < 5; iter++) {
    outCollStr = "mr_replace_col_" + iter;
    outDbStr = "mr_db_" + iter;
    print("Testing mr replace into DB " + iter)
    res = db.foo.mapReduce( map , reduce , { out : { replace: outCollStr, db: outDbStr } } )
    printjson(res);
    outDb = s.getDB(outDbStr);
    outColl = outDb[outCollStr];
    obj = outColl.convertToSingleObject("value");
    assert.eq( obj.count , 25600 , "Received wrong result " + obj.count );
    print("checking result field");
    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
}

for (i = 0; i < 5; i++) { print(i); }
s.stop()

