
t = db.mr5;
t.drop();

t.save( { "partner" : 1, "visits" : 9 } )
t.save( { "partner" : 2, "visits" : 9 } )
t.save( { "partner" : 1, "visits" : 11 } )
t.save( { "partner" : 1, "visits" : 30 } ) 
t.save( { "partner" : 2, "visits" : 41 } )
t.save( { "partner" : 2, "visits" : 41 } )

m = function(){
    emit( this.partner , { stats : [ this.visits ] } )
}

r = function( k , v ){
    var stats = [];
    var total = 0;
    for ( var i=0; i<v.length; i++ ){
        for ( var j in v[i].stats ) {
            stats.push( v[i].stats[j] )
            total += v[i].stats[j];
        }
    }
    return { stats : stats , total : total }
}

res = t.mapReduce( m , r , { scope : { xx : 1 } } );
res.find().forEach( printjson )

z = res.convertToSingleObject()
assert.eq( 2 , z.keySet().length , "A" )
assert.eq( [ 9 , 11 , 30 ] , z["1"].stats , "B" )
assert.eq( [ 9 , 41 , 41 ] , z["2"].stats , "B" )


res.drop()


