
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
x.find().forEach( printjson );
z = {};
x.find().forEach( function(a){ z[a.key] = a.value.count; } );
printjson( z );
assert.eq( 3 , z.keySet().length , "C" );
assert.eq( 2 , z.a , "D" );
assert.eq( 3 , z.b , "E" );
assert.eq( 3 , z.c , "F" );
x.drop();

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , query : { x : { "$gt" : 2 } } } );
assert.eq( 2 , res.numObjects , "B" );
x = db[res.result];
z = {};
x.find().forEach( function(a){ z[a.key] = a.value.count; } );
assert.eq( 1 , z.a , "C1" );
assert.eq( 1 , z.b , "C2" );
assert.eq( 2 , z.c , "C3" );
x.drop();

res = db.runCommand( { mapreduce : "mr1" , out : "foo" , map : m , reduce : r , query : { x : { "$gt" : 2 } } } );
assert.eq( 2 , res.numObjects , "B2" );
assert.eq( "foo" , res.result , "B2-c" );
x = db[res.result];
z = {};
x.find().forEach( function(a){ z[a.key] = a.value.count; } );
assert.eq( 1 , z.a , "C1a" );
assert.eq( 1 , z.b , "C2a" );
assert.eq( 2 , z.c , "C3a" );
x.drop();



for ( i=5; i<1000; i++ ){
    t.save( { x : i , tags : [ "b" , "d" ] } );
}

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r } );
printjson( res );
assert.eq( 999 , res.numObjects , "Z1" );
x = db[res.result];
x.find().forEach( printjson )
assert.eq( 4 , x.find().count() , "Z2" );
assert.eq( "a,b,c,d" , x.distinct( "key" ) , "Z3" );
assert.eq( 2 , x.findOne( { key : "a" } ).value.count , "ZA" );
assert.eq( 998 , x.findOne( { key : "b" } ).value.count , "ZB" );
assert.eq( 3 , x.findOne( { key : "c" } ).value.count , "ZC" );
assert.eq( 995 , x.findOne( { key : "d" } ).value.count , "ZD" );

print( Date.timeFunc( 
    function(){
        db.runCommand( { mapreduce : "mr1" , map : m , reduce : r } );
    } , 10 ) );    
