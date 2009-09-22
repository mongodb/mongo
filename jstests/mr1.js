
t = db.mr1;
t.drop();

t.save( { x : 1 , tags : [ "a" , "b" ] } );
t.save( { x : 2 , tags : [ "b" , "c" ] } );
t.save( { x : 3 , tags : [ "c" , "a" ] } );
t.save( { x : 4 , tags : [ "b" , "c" ] } );

emit = printjson;

m = function(){
    this.tags.forEach(
        function(z){
            emit( z , { count : 1 } );
        }
    );
};

r = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ )
        total += values[i].count;
    return { count : total };
};

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r } );
assert.eq( 4 , res.numObjects , "A" );
x = db[res.result];

assert.eq( 3 , x.find().count() , "B" );
z = {};
x.find().forEach( function(a){ z[a.key] = a.value.count; } );
assert.eq( 3 , z.keySet().length , "C" );
assert.eq( 2 , z.a , "D" );
assert.eq( 3 , z.b , "E" );
assert.eq( 3 , z.c , "F" );

x.drop();



