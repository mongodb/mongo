
t = db.mr_merge;
t.drop();

t.insert( { a : [ 1 , 2 ] } )
t.insert( { a : [ 2 , 3 ] } )
t.insert( { a : [ 3 , 4 ] } )

outName = "mr_merge_out";
out = db[outName];
out.drop();

m = function(){ for (i=0; i<this.a.length; i++ ) emit( this.a[i] , 1 ); } 
r = function(k,vs){ return Array.sum( vs ); }

function tos( o ){
    var s = "";
    for ( var i=0; i<100; i++ ){
        if ( o[i] )
            s += i + "_" + o[i];
    }
    return s;
}


res = t.mapReduce( m , r , { out : outName } )


expected = { "1" : 1 , "2" : 2 , "3" : 2 , "4" : 1 }
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "A" );

t.insert( { a : [ 4 , 5 ] } )
out.insert( { _id : 10 , value : "5" } )
res = t.mapReduce( m , r , { out : outName } )

expected["4"]++;
expected["5"] = 1
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "B" );

t.insert( { a : [ 5 , 6 ] } )
out.insert( { _id : 10 , value : "5" } )
res = t.mapReduce( m , r , { out : { merge : outName } } )

expected["5"]++;
expected["10"] = 5
expected["6"] = 1

assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "C" );

// test that the nonAtomic output gives valid result
t.insert( { a : [ 6 , 7 ] } )
out.insert( { _id : 20 , value : "10" } )
res = t.mapReduce( m , r , { out : { merge : outName, nonAtomic: true } } )

expected["6"]++;
expected["20"] = 10
expected["7"] = 1

assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "D" );

