
t = db.mr_outreduce;
t.drop();

t.insert( { _id : 1 , a : [ 1 , 2 ] } )
t.insert( { _id : 2 , a : [ 2 , 3 ] } )
t.insert( { _id : 3 , a : [ 3 , 4 ] } )

outName = "mr_outreduce_out";
out = db[outName];
out.drop();

m = function(){ for (i=0; i<this.a.length; i++ ) emit( this.a[i] , 1 ); } 
r = function(k,vs){ return Array.sum( vs ); }

function tos( o ){
    var s = "";
    for ( var i=0; i<100; i++ ){
        if ( o[i] )
            s += i + "_" + o[i] + "|"
    }
    return s;
}


res = t.mapReduce( m , r , { out : outName } )


expected = { "1" : 1 , "2" : 2 , "3" : 2 , "4" : 1 }
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "A" );

t.insert( { _id : 4 , a : [ 4 , 5 ] } )
out.insert( { _id : 10 , value : "5" } ) // this is a sentinal to make sure it wasn't killed
res = t.mapReduce( m , r , { out : { reduce : outName } , query : { _id : { $gt : 3 } } } )

expected["4"]++;
expected["5"] = 1
expected["10"] = 5
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "B" );

t.insert( { _id : 5 , a : [ 5 , 6 ] } )
out.insert( { _id : 20 , value : "10" } ) // this is a sentinal to make sure it wasn't killed
res = t.mapReduce( m , r , { out : { reduce : outName, nonAtomic: true } , query : { _id : { $gt : 4 } } } )

expected["5"]++;
expected["6"] = 1
expected["20"] = 10
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "C" );

