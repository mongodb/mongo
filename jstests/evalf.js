// test that killing a parent op interrupts the child op

t = db.jstests_evalf;
t.drop();

db.eval( function() {
        opid = null;
        while( opid == null ) {
            ops = db.currentOp().inprog;
            for( i in ops ) {
                o = ops[ i ];
                if ( o.active && o.query && o.query.$eval ) {
                    opid = o.opid;
                }
            }
        }
        db.jstests_evalf.save( {opid:opid} );
        db.jstests_evalf.count( { $where:function() {
                               db.killOp( db.jstests_evalf.findOne().opid );
                               while( 1 ) { ; }
                               } } );
        } );

