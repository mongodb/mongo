
t = db.cursora



function run( n ){

    t.drop()
    
    for ( i=0; i<n; i++ )
        t.insert( { _id : i } )
    db.getLastError()

    join = startParallelShell( "sleep(50); db.cursora.remove( {} );" );
    
    start = new Date()
    num = t.find( function(){ num = 2; for ( var x=0; x<1000; x++ ) num += 2; return num > 0; } ).sort( { _id : -1 } ).limit(n).itcount()
    end = new Date()

    join()

    print( "num: " + num + " time:" + ( end.getTime() - start.getTime() ) )
    assert.eq( 0 , t.count() , "after remove" )
}

//run( 5000 )
    

