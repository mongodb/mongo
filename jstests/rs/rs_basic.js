// rs_basic.js

load("../../jstests/rs/test_framework.js");

function go() {
    assert(__nextPort == 27000, "_nextPort==27000");

    a = null;
    try {init
        a = new Mongo("localhost:27000");
        print("using already open mongod on port 27000 -- presume you are debugging or something. should start empty.");
        __nextPort++;
    }
    catch (e) {
        a = rs_mongod();
    }

    b = rs_mongod();

    x = a.getDB("admin");
    y = b.getDB("admin");
    memb = [];
    memb[0] = x;
    memb[1] = y;

    print("rs_basic.js go(): started 2 servers");

    cfg = { _id: 'asdf', members: [] };
    var hn = hostname();
    cfg.members[0] = { _id: 0, host: hn + ":27000" };
    cfg.members[1] = { _id: 1, host: hn + ":27001" };

    print("cfg=" + tojson(cfg));
}

function init(server) {
    var i = server;
    //i = Random.randInt(2); // a random member of the set
    var m = memb[i];
    assert(!m.ismaster(), "not ismaster");
    var res = m.runCommand({ replSetInitiate: cfg });
    return res;
}

mongodPath = '../../db/Debug/';
print("Setting current directory to " + mongodPath);
cd(mongodPath);

print("go() to run");
print("init() to initiate");


/*
var rt = new ReplTest( "basic1" );

m = rt.start( true );
s = rt.start( false );

function block(){
    am.runCommand( { getlasterror : 1 , w : 2 , wtimeout : 3000 } )
}

am = m.getDB( "foo" );
as = s.getDB( "foo" );

function check( note ){
    var start = new Date();
    var x,y;
    while ( (new Date()).getTime() - start.getTime() < 30000 ){
        x = am.runCommand( "dbhash" );
        y = as.runCommand( "dbhash" );
        if ( x.md5 == y.md5 )
            return;
        sleep( 200 );
    }
    assert.eq( x.md5 , y.md5 , note );
}

am.a.save( { x : 1 } );
check( "A" );

am.a.save( { x : 5 } );

am.a.update( {} , { $inc : { x : 1 } } );
check( "B" );

am.a.update( {} , { $inc : { x : 1 } } , false , true );
check( "C" );

// -----   check features -------

// map/reduce
am.mr.insert( { tags : [ "a" ] } )
am.mr.insert( { tags : [ "a" , "b" ] } )
am.getLastError();
check( "mr setup" );

m = function(){
    for ( var i=0; i<this.tags.length; i++ ){
        print( "\t " + i );
        emit( this.tags[i] , 1 );
    }
}

r = function( key , v ){
    return Array.sum( v );
}

correct = { a : 2 , b : 1 };

function checkMR( t ){
    var res = t.mapReduce( m , r , "xyz" );
    assert.eq( correct , res.convertToSingleObject() , "checkMR: " + tojson( t ) );
}

function checkNumCollections( msg , diff ){
    if ( ! diff ) diff = 0;
    var m = am.getCollectionNames();
    var s = as.getCollectionNames();
    assert.eq( m.length + diff , s.length , "lengths bad \n" + tojson( m ) + "\n" + tojson( s ) );
}

checkNumCollections( "MR1" );
checkMR( am.mr );
checkMR( as.mr );
checkNumCollections( "MR2" );

block();
checkNumCollections( "MR3" );

var res = am.mr.mapReduce( m , r , { out : "xyz" } );
block();

checkNumCollections( "MR4" );


t = am.rpos;
t.insert( { _id : 1 , a : [ { n : "a" , c : 1 } , { n : "b" , c : 1 } , { n : "c" , c : 1 } ] , b : [ 1 , 2 , 3 ] } )
block();
check( "after pos 1 " );

t.update( { "a.n" : "b" } , { $inc : { "a.$.c" : 1 } } )
block();
check( "after pos 2 " );

t.update( { "b" : 2 } , { $inc : { "b.$" : 1 } } )
block();
check( "after pos 3 " );

t.update( { "b" : 3} , { $set : { "b.$" : 17 } } )
block();
check( "after pos 4 " );


printjson( am.rpos.findOne() )
printjson( as.rpos.findOne() )

//am.getSisterDB( "local" ).getCollection( "oplog.$main" ).find().limit(10).sort( { $natural : -1 } ).forEach( printjson )

t = am.b;
t.update( { "_id" : "fun"}, { $inc : {"a.b.c.x" : 6743} } , true, false)
block()
check( "b 1" );

t.update( { "_id" : "fun"}, { $inc : {"a.b.c.x" : 5} } , true, false)
block()
check( "b 2" );

t.update( { "_id" : "fun"}, { $inc : {"a.b.c.x" : 100, "a.b.c.y" : 911} } , true, false)
block()
assert.eq( { _id : "fun" , a : { b : { c : { x : 6848 , y : 911 } } } } , as.b.findOne() , "b 3" );
//printjson( t.findOne() )
//printjson( as.b.findOne() )
//am.getSisterDB( "local" ).getCollection( "oplog.$main" ).find().sort( { $natural : -1 } ).limit(3).forEach( printjson )
check("b 4");

rt.stop();
*/
