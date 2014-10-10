
t = db.geo9
t.drop();

t.save( { _id : 1 , a : [ 10 , 10 ] , b : [ 50 , 50 ] } )
t.save( { _id : 2 , a : [ 11 , 11 ] , b : [ 51 , 52 ] } )
t.save( { _id : 3 , a : [ 12 , 12 ] , b : [ 52 , 52 ] } )

t.save( { _id : 4 , a : [ 50 , 50 ] , b : [ 10 , 10 ] } )
t.save( { _id : 5 , a : [ 51 , 51 ] , b : [ 11 , 11 ] } )
t.save( { _id : 6 , a : [ 52 , 52 ] , b : [ 12 , 12 ] } )

t.ensureIndex( { a : "2d" } )
t.ensureIndex( { b : "2d" } )

function check( field ){
    var q = {}
    q[field] = { $near : [ 11 , 11 ] }
    arr = t.find( q ).limit(3).map( 
        function(z){
            return Geo.distance( [ 11 , 11 ] , z[field] );
        }
    );
    assert.eq( 2 * Math.sqrt( 2 ) , Array.sum( arr ) , "test " + field );
}

check( "a" )
check( "b" )
