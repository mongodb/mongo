// test that killing a parent op interrupts the child op

t = db.jstests_evalf;
t.drop();

//if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode - SERVER-1900

// the code in eval must be under 512 chars because otherwise it's not displayed in curOp()
try {
db.eval( function() {
        opid = null;
        while( opid == null ) {
            ops = db.currentOp().inprog;
            for( i in ops ) {
                o = ops[ i ];
                if ( o.active && o.query && o.query.$eval ) { opid = o.opid; }
            }}
        db.jstests_evalf.save( {"opid":opid} );
        db.jstests_evalf.count( { $where:function() { var id = db.jstests_evalf.findOne().opid; db.killOp( id ); while( 1 ) { ; } } } );
        } );
} catch (ex) {
    // exception is thrown in V8 when job gets killed. Does not seem like bad behavior.
}

// make sure server and JS still work
db.eval( function() { db.jstests_evalf.count(); });
//}
