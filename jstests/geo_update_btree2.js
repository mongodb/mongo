// Tests whether the geospatial search is stable under btree updates

var status = function( msg ){
    print( "\n\n###\n" + msg + "\n###\n\n" )
}

var coll = db.getCollection( "jstests_geo_update_btree2" )
coll.drop()

coll.ensureIndex( { loc : '2d' } )

status( "Inserting points..." )

var numPoints = 10
for ( i = 0; i < numPoints; i++ ) {
    coll.insert( { _id : i, loc : [ Random.rand() * 180, Random.rand() * 180 ], i : i % 2 } );
}

status( "Starting long query..." )

var query = coll.find({ loc : { $within : { $box : [[-180, -180], [180, 180]] } } }).batchSize( 2 )
var firstValues = [ query.next()._id, query.next()._id ]
printjson( firstValues )

status( "Removing points not returned by query..." )

var allQuery = coll.find()
var removeIds = []
while( allQuery.hasNext() ){
    var id = allQuery.next()._id
    if( firstValues.indexOf( id ) < 0 ){
        removeIds.push( id )
    }
}

var updateIds = []
for( var i = 0, max = removeIds.length / 2; i < max; i++ ) updateIds.push( removeIds.pop() )

printjson( removeIds )
coll.remove({ _id : { $in : removeIds } })

status( "Updating points returned by query..." )

var big = new Array( 3000 ).toString()
for( var i = 0; i < updateIds.length; i++ ) 
    coll.update({ _id : updateIds[i] }, { $set : { data : big } })

status( "Counting final points..." )

assert.eq( ( numPoints - 2 ) / 2, query.itcount() )


