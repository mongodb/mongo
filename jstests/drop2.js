t = db.jstests_drop2;
t.drop();

function debug( x ) {
    printjson( x );
}

t.save( {} );
db.getLastError();

function op( drop ) {
    p = db.currentOp().inprog;
    debug( p );
    for ( var i in p ) {
        var o = p[ i ];
        if ( drop ) {
            if (  o.query && o.query.drop && o.query.drop == "jstests_drop2" ) {
                return o.opid;
            }
        } else {
            if (  o.query && o.query.query && o.query.query.$where && o.ns == "test.jstests_drop2" ) {
                return o.opid;
            }
        }
    }
    return null;
}

s1 = startParallelShell( "print(\"Count thread started\");"
                         + "db.jstests_drop2.count( { $where: function() {"
                         + "while( 1 ) { sleep( 1 ); } } } );"
                         + "print(\"Count thread terminating\");" );
countOp = null;
assert.soon( function() { countOp = op( false ); return countOp; } );

s2 = startParallelShell( "print(\"Drop thread started\");"
                         + "print(\"drop result: \" + db.jstests_drop2.drop() );"
                         + "print(\"Drop thread terminating\")" );
dropOp = null;
assert.soon( function() { dropOp = op( true ); return dropOp; } );

db.killOp( dropOp );
db.killOp( countOp );

s1();
s2();

t.drop(); // in SERVER-1818, this fails
