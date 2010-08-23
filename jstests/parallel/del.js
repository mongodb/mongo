

N = 1000;

HOST = db.getMongo().host

DONE = false;

function del1( dbname ){
    var m = new Mongo( HOST )
    var db = m.getDB( "foo" + dbname );
    var t = db.del

    while ( ! DONE ){
        var r = Math.random();
        var n = Math.floor( Math.random() * N );
        if ( r < .9 ){
            t.insert( { x : n } )
        }
        else if ( r < .98 ){
            t.remove( { x : n } );
        }
        else if ( r < .99 ){
            t.remove( { x : { $lt : n }  } )
        }
        else {
            t.remove( { x : { $gt : n } } );
        }
        if ( r > .9999 )
            print( t.count() )
    }
}

function del2( dbname ){
    var m = new Mongo( HOST )
    var db = m.getDB( "foo" + dbname );
    var t = db.del

    while ( ! DONE ){
        var r = Math.random();
        var n = Math.floor( Math.random() * N );
        var s = Math.random() > .5 ? 1 : -1;
        
        if ( r < .5 ){
            t.findOne( { x : n } )
        }
        else if ( r < .75 ){
            t.find( { x : { $lt : n } } ).sort( { x : s } ).itcount();
        }
        else {
            t.find( { x : { $gt : n } } ).sort( { x : s } ).itcount();
        }
    }
}

all = []

all.push( fork( del1 , "a" ) )
all.push( fork( del2 , "a" ) )
all.push( fork( del1 , "b" ) )
all.push( fork( del2 , "b" ) )

for ( i=0; i<all.length; i++ )
    all[i].start()

a = db.getSisterDB( "fooa" )
b = db.getSisterDB( "foob" )

for ( i=0; i<10; i++ ){
    sleep( 2000 )
    print( "dropping" )
    a.dropDatabase();
    b.dropDatabase();
}

DONE = true;

all[0].join()

