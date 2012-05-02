
N = 1000;
HOST = db.getMongo().host

a = db.getSisterDB( "fooa" )
b = db.getSisterDB( "foob" )
a.dropDatabase();
b.dropDatabase();

function del1( dbname, host, max ){
    var m = new Mongo( host )
    var db = m.getDB( "foo" + dbname );
    var t = db.del

    while ( !db.del_parallel.count() ){
        var r = Math.random();
        var n = Math.floor( Math.random() * max );
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

function del2( dbname, host, max ){
    var m = new Mongo( host )
    var db = m.getDB( "foo" + dbname );
    var t = db.del

    while ( !db.del_parallel.count() ){
        var r = Math.random();
        var n = Math.floor( Math.random() * max );
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

all.push( fork( del1 , "a", HOST, N ) )
all.push( fork( del2 , "a", HOST, N ) )
all.push( fork( del1 , "b", HOST, N ) )
all.push( fork( del2 , "b", HOST, N ) )

for ( i=0; i<all.length; i++ )
    all[i].start()

for ( i=0; i<10; i++ ){
    sleep( 2000 )
    print( "dropping" )
    a.dropDatabase();
    b.dropDatabase();
}

a.del_parallel.save({done: 1})
b.del_parallel.save({done: 1})

all[0].join()

