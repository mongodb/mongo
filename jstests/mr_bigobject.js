
t = db.mr_bigobject
t.drop()

// v8 requires large start string, otherwise UTF16
var large = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
var s = large;
while ( s.length < ( 6 * 1024 * 1024 ) ){
    s += large;
}

for ( i=0; i<5; i++ )
    t.insert( { _id : i , s : s } )

m = function(){
    emit( 1 , this.s + this.s );
}

r = function( k , v ){
    return 1;
}

assert.throws( function(){ r = t.mapReduce( m , r , "mr_bigobject_out" ); } , null , "emit should fail" )


m = function(){
    emit( 1 , this.s );
}

assert.eq( { 1 : 1 } , t.mapReduce( m , r , "mr_bigobject_out" ).convertToSingleObject() , "A1" )

r = function( k , v ){
    total = 0;
    for ( var i=0; i<v.length; i++ ){
        var x = v[i];
        if ( typeof( x ) == "number" )
            total += x
        else
            total += x.length;
    }
    return total;
}

assert.eq( { 1 : t.count() * s.length } , t.mapReduce( m , r , "mr_bigobject_out" ).convertToSingleObject() , "A1" )

t.drop()
