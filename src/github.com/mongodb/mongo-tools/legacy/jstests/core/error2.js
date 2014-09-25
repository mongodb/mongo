// Test that client gets stack trace on failed invoke

f = db.jstests_error2;

f.drop();

f.save( {a:1} );

assert.throws( 
    function(){
        c = f.find({$where : function(){ return a() }});
        c.next();
    }
);

assert.throws(
    function(){
        db.eval( function() { return a(); } );
    }
);

