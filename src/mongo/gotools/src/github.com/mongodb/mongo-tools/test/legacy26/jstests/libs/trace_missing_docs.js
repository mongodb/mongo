
//
// On error inserting documents, traces back and shows where the document was dropped
//

function traceMissingDoc( coll, doc, mongos ) {

    if (mongos) coll = mongos.getCollection(coll + "");
    else mongos = coll.getMongo();
    
    var config = mongos.getDB( "config" );
    var shards = config.shards.find().toArray();
    for ( var i = 0; i < shards.length; i++ ) {
        shards[i].conn = new Mongo( shards[i].host );
    }
    
    var shardKeyPatt = config.collections.findOne({ _id : coll + "" }).key;
    
    // Project out the shard key
    var shardKey = {};
    for ( var k in shardKeyPatt ) {
        if ( doc[k] == undefined ) {
            jsTest.log( "Shard key " + tojson( shardKey ) + 
                        " not found in doc " + tojson( doc ) + 
                        ", falling back to _id search..." );
            shardKeyPatt = { _id : 1 };
            shardKey = { _id : doc['_id'] };
            break;
        }
        shardKey[k] = doc[k];
    }
    
    if ( doc['_id'] == undefined ) {
        jsTest.log( "Id not found in doc " + tojson( doc ) + " cannot trace oplog entries." );
        return;
    }
    
    jsTest.log( "Using shard key : " + tojson( shardKey ) );
    
    var allOps = [];
    for ( var i = 0; i < shards.length; i++ ) {
        
        var oplog = shards[i].conn.getCollection( "local.oplog.rs" );
        if ( !oplog.findOne() ) {
            oplog = shards[i].conn.getCollection( "local.oplog.$main" );
        }
        
        if ( !oplog.findOne() ) {
            jsTest.log( "No oplog was found on shard " + shards[i]._id );
            continue;
        }
        
        var addKeyQuery = function( query, prefix ) {
            for ( var k in shardKey ) {
                query[prefix + '.' + k] = shardKey[k];
            }
            return query;
        };
        
        var addToOps = function( cursor ) {
            cursor.forEach( function( doc ) { 
                doc.shard = shards[i]._id;
                doc.realTime = new Date( doc.ts.getTime() * 1000 );
                allOps.push( doc );
            });
        };

        // Find ops
        addToOps( oplog.find( addKeyQuery( { op : 'i' }, 'o' ) ) );
        var updateQuery = { $or : [ addKeyQuery( { op : 'u' }, 'o2' ),
                                    { op : 'u', 'o2._id' : doc['_id'] } ] };
        addToOps( oplog.find( updateQuery ) );
        addToOps( oplog.find({ op : 'd', 'o._id' : doc['_id'] }) );
    }
    
    var compareOps = function( opA, opB ) {
        if ( opA.ts < opB.ts ) return -1;
        if ( opB.ts < opA.ts ) return 1;
        else return 0;
    }
    
    allOps.sort( compareOps );
    
    print( "Ops found for doc " + tojson( doc ) + " on each shard:\n" );
    for ( var i = 0; i < allOps.length; i++ ) {
        printjson( allOps[i] );
    }
    
    return allOps;
}