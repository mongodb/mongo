

t = db.mr2;
t.drop();

t.save( { comments : [ { who : "a" , txt : "asdasdasd" } ,
                       { who : "b" , txt : "asdasdasdasdasdasdas" } ] } );

t.save( { comments : [ { who : "b" , txt : "asdasdasdaaa" } ,
                       { who : "c" , txt : "asdasdasdaasdasdas" } ] } );



function m(){
    for ( var i=0; i<this.comments.length; i++ ){
        var c = this.comments[i];
        emit( c.who , { totalSize : c.txt.length , num : 1 } );
    }
}

function r( who , values ){
    var n = { totalSize : 0 , num : 0 };
    for ( var i=0; i<values.length; i++ ){
        n.totalSize += values[i].totalSize;
        n.num += values[i].num;
    }
    return n;
}

function reformat( r ){
    var x = {};
    var cursor;
    if ( r.results )
        cursor = r.results;
    else
        cursor = r.find();
    cursor.forEach( 
        function(z){
            x[z._id] = z.value;
        }
    );
    return x;
}

function f( who , res ){
    res.avg = res.totalSize / res.num;
    return res;
}

res = t.mapReduce( m , r , { finalize : f , out : "mr2_out" } );
printjson( res )
x = reformat( res );
assert.eq( 9 , x.a.avg , "A1" );
assert.eq( 16 , x.b.avg , "A2" );
assert.eq( 18 , x.c.avg , "A3" );
res.drop();

res = t.mapReduce( m , r , { finalize : f , out : { inline : 1 } } );
printjson( res )
x = reformat( res );
assert.eq( 9 , x.a.avg , "B1" );
assert.eq( 16 , x.b.avg , "B2" );
assert.eq( 18 , x.c.avg , "B3" );
res.drop();

assert( ! ( "result" in res ) , "B4" )

var final2 = function(key, res) {
    res.avg = res.totalSize / res.num;
    if (res.avg < 18) return null;
    return res;
}
res = t.mapReduce( m , r , { finalize : final2 , out : "mr2_out" } );
printjson( res )
x = reformat( res );
assert( x.a == undefined , "C1" );
assert( x.b == undefined , "C2" );
assert.eq( 18 , x.c.avg , "C3" );
res.drop();

var final2 = function(key, res) {
    return null;
}
res = t.mapReduce( m , r , { finalize : final2 , out : { inline : 1 } } );
printjson( res )
x = reformat( res );
assert( x.a == undefined , "D1" );
assert( x.b == undefined , "D2" );
assert( x.c == undefined , "D3" );
res.drop();

