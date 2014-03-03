// Make sure many locations in one doc works, in the form of an array

t = db.geoarray1
function test(index) {
    t.drop();

    var locObj = []
        // Add locations everywhere
        for ( var i = 0; i < 10; i++ ) {
            for ( var j = 0; j < 10; j++ ) {
                if ( j % 2 == 0 )
                    locObj.push( [ i, j ] )
                else
                    locObj.push( { x : i, y : j } )
            }
        }

    // Add docs with all these locations
    for( var i = 0; i < 300; i++ ){
        t.insert( { loc : locObj } )
    }

    if (index) {
        t.ensureIndex( { loc : "2d" } )
    }

    // Pull them back
    for ( var i = 0; i < 10; i++ ) {
        for ( var j = 0; j < 10; j++ ) {
            assert.eq(300, t.find({loc: {$within: {$box: [[i - 0.5, j - 0.5 ],
                                                          [i + 0.5,j + 0.5]]}}})
                      .count())
        }
    }
}

test(true)
test(false)
