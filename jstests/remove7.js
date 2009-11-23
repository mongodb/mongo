
t = db.remove7
t.drop();



function getTags( n ){
    n = n || 5;
    var a = [];
    for ( var i=0; i<n; i++ ){
        var v = Math.ceil( 20 * Math.random() );
        a.push( v );
    }

    return a;
}

for ( i=0; i<1000; i++ ){
    t.save( { tags : getTags() } );
}

t.ensureIndex( { tags : 1 } );

for ( i=0; i<200; i++ ){
    for ( var j=0; j<10; j++ )
        t.save( { tags : getTags( 100 ) } );
    //t.remove( { tags : { $in : getTags( 10 ) } } );
    var o = db.getLastErrorObj();
    assert.isnull( o.err , "error: " + tojson( o ) );
}
    
