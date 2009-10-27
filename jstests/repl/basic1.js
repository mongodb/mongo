
// test repl basics
// data on master/slave is the same

var rt = new ReplTest( "basic1" );

m = rt.start( true );
s = rt.start( false );

function hash( db ){
    var s = "";
    var a = db.getCollectionNames();
    a = a.sort();
    a.forEach(
        function(cn){
            var c = db.getCollection( cn );
            s += cn + "\t" + c.find().count() + "\n";
            c.find().sort( { _id : 1 } ).forEach(
                function(o){
                    s += tojson( o , "" , true ) + "\n";
                }
            );
        }
    );
    return s;
}

am = m.getDB( "foo" );
as = s.getDB( "foo" );

function check( note ){
    var start = new Date();
    var x,y;
    while ( (new Date()).getTime() - start.getTime() < 30000 ){
        x = hash( am );
        y = hash( as );
        if ( x == y )
            return;
        sleep( 200 );
    }
    assert.eq( x , y , note );
}

am.a.save( { x : 1 } );
check( "A" );

am.a.save( { x : 5 } );

am.a.update( {} , { $inc : { x : 1 } } );
check( "B" );

am.a.update( {} , { $inc : { x : 1 } } , false , true );
check( "C" );

rt.stop();




