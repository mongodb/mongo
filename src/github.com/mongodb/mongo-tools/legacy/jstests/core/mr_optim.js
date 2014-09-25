

t = db.mr_optim;
t.drop();

for (var i = 0; i < 1000; ++i) {
    t.save( {a: Math.random(1000), b: Math.random(10000)} );
}

function m(){
    emit(this._id, 13);
}

function r( key , values ){
    return "bad";
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

res = t.mapReduce( m , r , { out : "mr_optim_out" } );
printjson( res )
x = reformat( res );
for (var key in x) {
    assert.eq(x[key], 13, "value is not equal to original, maybe reduce has run");
}
res.drop();

res = t.mapReduce( m , r , { out : { inline : 1 } } );
//printjson( res )
x2 = reformat( res );
res.drop();

assert.eq(x, x2, "object from inline and collection are not equal")

t.drop();