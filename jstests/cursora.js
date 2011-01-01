t = db.cursora

function run( n , atomic ){
    if( !isNumber(n) ) {
	print("n:");
	printjson(n);
	assert(isNumber(n), "cursora.js isNumber");
    }
    t.drop()
    
    for ( i=0; i<n; i++ )
        t.insert( { _id : i } )
    db.getLastError()

    print("cursora.js startParallelShell n:"+n+" atomic:"+atomic)
    join = startParallelShell( "sleep(50); db.cursora.remove( {"  + ( atomic ? "$atomic:true" : "" ) + "} ); db.getLastError();" );

    var start = null;
    var ex = null;
    var num = null;
    var end = null;
    try {
        start = new Date()
        ex = t.find(function () { num = 2; for (var x = 0; x < 1000; x++) num += 2; return num > 0; }).sort({ _id: -1 }).explain()
        num = ex.n
        end = new Date()
    }
    catch (e) {
        print("cursora.js FAIL " + e);
        join();
        throw e;
    }
    
    join()

    //print( "cursora.js num: " + num + " time:" + ( end.getTime() - start.getTime() ) )
    assert.eq( 0 , t.count() , "after remove: " + tojson( ex ) )
    // assert.lt( 0 , ex.nYields , "not enough yields : " + tojson( ex ) ); // TODO make this more reliable so cen re-enable assert
    if ( n == num )
        print( "cursora.js warning: shouldn't have counted all  n: " + n + " num: " + num );
}

run( 1500 )
run( 5000 )
run( 1500 , true )
run( 5000 , true )
print("cursora.js SUCCESS")
