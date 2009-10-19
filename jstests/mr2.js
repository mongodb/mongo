

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
    r.find().forEach( 
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
res = t.mapReduce( m , r , { finalize : f } );
x = reformat( res );
assert.eq( 9 , x.a.avg , "A" );
assert.eq( 16 , x.b.avg , "B" );
assert.eq( 18 , x.c.avg , "C" );
res.drop();

