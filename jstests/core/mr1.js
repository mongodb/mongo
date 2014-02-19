
t = db.mr1;
t.drop();

t.save( { x : 1 , tags : [ "a" , "b" ] } );
t.save( { x : 2 , tags : [ "b" , "c" ] } );
t.save( { x : 3 , tags : [ "c" , "a" ] } );
t.save( { x : 4 , tags : [ "b" , "c" ] } );

emit = printjson;

function d( x ){
    printjson( x );
}

ks = "_id";
if ( db.version() == "1.1.1" )
    ks = "key";


m = function(){
    this.tags.forEach(
        function(z){
            emit( z , { count : 1 } );
        }
    );
};

m2 = function(){
    for ( var i=0; i<this.tags.length; i++ ){
        emit( this.tags[i] , 1 );
    }
};


r = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ ){
        total += values[i].count;
    }
    return { count : total };
};

r2 = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ ){
        total += values[i];
    }
    return total;
};

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , out : "mr1_out" } );
d( res );
if ( ks == "_id" ) assert( res.ok , "not ok" );
assert.eq( 4 , res.counts.input , "A" );
x = db[res.result];

assert.eq( 3 , x.find().count() , "B" );
x.find().forEach( d );
z = {};
x.find().forEach( function(a){ z[a[ks]] = a.value.count; } );
d( z );
assert.eq( 3 , Object.keySet( z ).length , "C" );
assert.eq( 2 , z.a , "D" );
assert.eq( 3 , z.b , "E" );
assert.eq( 3 , z.c , "F" );
x.drop();

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , query : { x : { "$gt" : 2 } } , out : "mr1_out" } );
d( res );
assert.eq( 2 , res.counts.input , "B" );
x = db[res.result];
z = {};
x.find().forEach( function(a){ z[a[ks]] = a.value.count; } );
assert.eq( 1 , z.a , "C1" );
assert.eq( 1 , z.b , "C2" );
assert.eq( 2 , z.c , "C3" );
x.drop();

res = db.runCommand( { mapreduce : "mr1" , map : m2 , reduce : r2 , query : { x : { "$gt" : 2 } } , out : "mr1_out" } );
d( res );
assert.eq( 2 , res.counts.input , "B" );
x = db[res.result];
z = {};
x.find().forEach( function(a){ z[a[ks]] = a.value; } );
assert.eq( 1 , z.a , "C1z" );
assert.eq( 1 , z.b , "C2z" );
assert.eq( 2 , z.c , "C3z" );
x.drop();

res = db.runCommand( { mapreduce : "mr1" , out : "mr1_foo" , map : m , reduce : r , query : { x : { "$gt" : 2 } } } );
d( res );
assert.eq( 2 , res.counts.input , "B2" );
assert.eq( "mr1_foo" , res.result , "B2-c" );
x = db[res.result];
z = {};
x.find().forEach( function(a){ z[a[ks]] = a.value.count; } );
assert.eq( 1 , z.a , "C1a" );
assert.eq( 1 , z.b , "C2a" );
assert.eq( 2 , z.c , "C3a" );
x.drop();

for ( i=5; i<1000; i++ ){
    t.save( { x : i , tags : [ "b" , "d" ] } );
}

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , out : "mr1_out" } );
d( res );
assert.eq( 999 , res.counts.input , "Z1" );
x = db[res.result];
x.find().forEach( d )
assert.eq( 4 , x.find().count() , "Z2" );
assert.eq( "a,b,c,d" , x.distinct( ks ) , "Z3" );

function getk( k ){
    var o = {};
    o[ks] = k;
    return x.findOne( o );
}

assert.eq( 2 , getk( "a" ).value.count , "ZA" );
assert.eq( 998 , getk( "b" ).value.count , "ZB" );
assert.eq( 3 , getk( "c" ).value.count , "ZC" );
assert.eq( 995 , getk( "d" ).value.count , "ZD" );
x.drop();

if ( true ){
    printjson( db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , verbose : true , out : "mr1_out" } ) );
}

print( "t1: " + Date.timeFunc( 
    function(){
        var out = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , out : "mr1_out" } );
        if ( ks == "_id" ) assert( out.ok , "XXX : " + tojson( out ) );
        db[out.result].drop();
    } , 10 ) + " (~500 on 2.8ghz) - itcount: " + Date.timeFunc( function(){ db.mr1.find().itcount(); } , 10 ) );    



// test doesn't exist
res = db.runCommand( { mapreduce : "lasjdlasjdlasjdjasldjalsdj12e" , map : m , reduce : r , out : "mr1_out" } );
assert( ! res.ok , "should be not ok" );

if ( true ){
    correct = {};
    
    for ( i=0; i<20000; i++ ){
        k = "Z" + i % 10000;
        if ( correct[k] )
            correct[k]++;
        else
            correct[k] = 1;
        t.save( { x : i , tags : [ k ] } );
    }
    
    res = db.runCommand( { mapreduce : "mr1" , out : "mr1_foo" , map : m , reduce : r } );
    d( res );
    print( "t2: " + res.timeMillis + " (~3500 on 2.8ghz) - itcount: " + Date.timeFunc( function(){ db.mr1.find().itcount(); } ) );
    x = db[res.result];
    z = {};
    x.find().forEach( function(a){ z[a[ks]] = a.value.count; } );
    for ( zz in z ){
        if ( zz.indexOf( "Z" ) == 0 ){
            assert.eq( correct[zz] , z[zz] , "ZZ : " + zz );
        }
    }
    x.drop();
    
    res = db.runCommand( { mapreduce : "mr1" , out : "mr1_foo" , map : m2 , reduce : r2 , out : "mr1_out" } );
    d(res);
    print( "t3: " + res.timeMillis + " (~3500 on 2.8ghz)" );

    res = db.runCommand( { mapreduce : "mr1" , map : m2 , reduce : r2 , out : { inline : true } } );
    print( "t4: " + res.timeMillis  );

}


res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r , out : "mr1_out" } );
assert( res.ok , "should be ok" );

t.drop();
t1 = db.mr1_out;
t1.drop();