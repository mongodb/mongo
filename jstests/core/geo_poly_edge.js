//
// Tests polygon edge cases
//

var coll = db.getCollection( 'jstests_geo_poly_edge' )
coll.drop();

coll.ensureIndex({ loc : "2d" })

coll.insert({ loc : [10, 10] })
coll.insert({ loc : [10, -10] })

assert.eq( coll.find({ loc : { $within : { $polygon : [[ 10, 10 ], [ 10, 10 ], [ 10, -10 ]] } } }).itcount(), 2 )

assert.eq( coll.find({ loc : { $within : { $polygon : [[ 10, 10 ], [ 10, 10 ], [ 10, 10 ]] } } }).itcount(), 1 )


coll.insert({ loc : [179, 0] })
coll.insert({ loc : [0, 179] })

assert.eq( coll.find({ loc : { $within : { $polygon : [[0, 0], [1000, 0], [1000, 1000], [0, 1000]] } } }).itcount(), 3 )

