
t = db.mr_merge2;
t.drop();

t.insert( { a : [ 1 , 2 ] } )
t.insert( { a : [ 2 , 3 ] } )
t.insert( { a : [ 3 , 4 ] } )

outName = "mr_merge2_out";
out = db[outName];
out.drop();

m = function(){ for (i=0; i<this.a.length; i++ ) emit( this.a[i] , 1 ); } 
r = function(k,vs){ return Array.sum( vs ); }

function tos( o ){
    var s = "";
    for ( var i=0; i<100; i++ ){
        if ( o[i] )
            s += i + "_" + o[i] + "|";
    }
    return s;
}


outOptions = { out : { merge : outName } }

res = t.mapReduce( m , r , outOptions )
expected = { "1" : 1 , "2" : 2 , "3" : 2 , "4" : 1 }
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "A" );

t.insert( { a : [ 4 , 5 ] } )
res = t.mapReduce( m , r , outOptions )
expected["4"]++;
expected["5"] = 1
assert.eq( tos( expected ) , tos( res.convertToSingleObject() ) , "B" );

