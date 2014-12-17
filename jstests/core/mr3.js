
t = db.mr3;
t.drop();

t.save( { x : 1 , tags : [ "a" , "b" ] } );
t.save( { x : 2 , tags : [ "b" , "c" ] } );
t.save( { x : 3 , tags : [ "c" , "a" ] } );
t.save( { x : 4 , tags : [ "b" , "c" ] } );

m = function( n , x ){
    x = x || 1;
    this.tags.forEach(
        function(z){
            for ( var i=0; i<x; i++ )
                emit( z , { count : n || 1 } );
        }
    );
};

r = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ ){
        total += values[i].count;
    }
    return { count : total };
};

res = t.mapReduce( m , r , { out : "mr3_out" } );
z = res.convertToSingleObject()

assert.eq( 3 , Object.keySet( z ).length , "A1" );
assert.eq( 2 , z.a.count , "A2" );
assert.eq( 3 , z.b.count , "A3" );
assert.eq( 3 , z.c.count , "A4" );

res.drop();

res = t.mapReduce( m , r , { out : "mr3_out" , mapparams : [ 2 , 2 ] } );
z = res.convertToSingleObject()

assert.eq( 3 , Object.keySet( z ).length , "B1" );
assert.eq( 8 , z.a.count , "B2" );
assert.eq( 12 , z.b.count , "B3" );
assert.eq( 12 , z.c.count , "B4" );

res.drop();

// -- just some random tests

realm = m;

m = function(){
    emit( this._id , 1 );
}
res = t.mapReduce( m , r , { out : "mr3_out" } );
res.drop();

m = function(){
    emit( this._id , this.xzz.a );
}

before = db.getCollectionNames().length;
assert.throws( function(){ t.mapReduce( m , r , { out : "mr3_out" } ); } );
assert.eq( before , db.getCollectionNames().length , "after throw crap" );


m = realm;
r = function( k , v ){
    return v.x.x.x;
}
before = db.getCollectionNames().length;
assert.throws( function(){ t.mapReduce( m , r , "mr3_out"  ) } )
assert.eq( before , db.getCollectionNames().length , "after throw crap" );
