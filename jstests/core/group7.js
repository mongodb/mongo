// Test yielding group command SERVER-1395

t = db.jstests_group7;
t.drop();

function checkForYield( docs, updates ) {
    t.drop();
    a = 0;
    for( var i = 0; i < docs; ++i ) {
        t.save( {a:a} );
    }

    // Iteratively update all a values atomically.
    p = startParallelShell(
        'for( a = 0; a < ' + updates + '; ++a ) {' +
            'db.jstests_group7.update({ $atomic: true }, { $set: { a: a }}, false, true);' +
        '}' );

    for( var i = 0; i < updates; ++i ) {
        print("running group " + i + " of " + updates);
        ret = t.group({key:{a:1},reduce:function(){},initial:{}});
        // Check if group sees more than one a value, indicating that it yielded.
        if ( ret.length > 1 ) {
            p();
            return true;
        }
        printjson( ret );
    }

    p();
    return false;
}

var yielded = false;
var docs = 1500;
var updates = 50;
for( var j = 1; j <= 6; ++j ) {
    print("Iteration " + j + " docs = " + docs + " updates = " + updates);
    if ( checkForYield( docs, updates ) ) {
        yielded = true;
        break;
    }
     // Increase docs and updates to encourage yielding.
    docs *= 2;
    updates *= 2;
}
assert( yielded );
