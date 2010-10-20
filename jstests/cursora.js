
t = db.cursora



function run( n , atomic ){

    t.drop()
    
    for ( i=0; i<n; i++ )
        t.insert( { _id : i } )
    db.getLastError()

    join = startParallelShell( "sleep(50); db.cursora.remove( {"  + ( atomic ? "$atomic:true" : "" ) + "} ); db.getLastError();" );
    
    start = new Date()
    num = t.find( function(){ num = 2; for ( var x=0; x<1000; x++ ) num += 2; return num > 0; } ).sort( { _id : -1 } ).limit(n).itcount()
    end = new Date()

    join()

    print( "num: " + num + " time:" + ( end.getTime() - start.getTime() ) )
    assert.eq( 0 , t.count() , "after remove" )
    if ( n == num )
        print( "warning: shouldn't have counted all  n: " + n + " num: " + num );
}

// run( 1500 )
// run( 5000 )

// run( 1500 , true )
// run( 5000 , true )
    

