

t = db.server4638
t.drop();

function make_blob(sz) {
    var x = "";
    while ( x.length < sz )
        x+= ".";
}

small_blob = make_blob( 250 )

for ( i=0; i<2; i++ ){
    //t.insert( { _id : i , x : i } ); // this version works
    t.insert( { _id : i , x : i , blob : small_blob , a : [] } );
}
db.getLastError();
res = t.aggregate( { $project : { x : 1 } } )//, { $group : { _id : "$x" , total : { $sum : 1 } } } )
printjson(res)

assert(res.ok, 'server4638 failed');

