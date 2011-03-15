// Randomized index testing with initial btree constructed using btree builder.
// Also uses large strings.

Random.setRandomSeed();

t = db.test_index_check10;

function doIt() {

t.drop();

function sort() {
    var sort = {};
    for( var i = 0; i < n; ++i ) {
        sort[ fields[ i ] ] = Random.rand() > 0.5 ? 1 : -1;
    }    
    return sort;
}

var fields = [ 'a', 'b', 'c', 'd', 'e' ];
n = Random.randInt( 5 ) + 1;
var idx = sort();

var chars = "abcdefghijklmnopqrstuvwxyz";
    
function obj() {
    var ret = {};
    for( var i = 0; i < n; ++i ) {
        ret[ fields[ i ] ] = r();
    }
    return ret;
}

function r() {
    var len = Random.randInt( 1000 / n );
    buf = "";
    for( var i = 0; i < len; ++i ) {
        buf += chars.charAt( Random.randInt( chars.length ) );
    }
    return buf;
}

// filter out docs with keys too big to be indexed
function lowPassFilter( a ) {
    var ret = [];
    for( var i in a ) {
        if ( Object.bsonsize( a[ i ] ) <= 819 /* KeyMax */ ) {
            ret.push( a[ i ] );
        }
    }
    return ret;
}
    
function check() {
    var v = t.validate();
    if ( !t.valid ) {
        printjson( t );
        assert( t.valid );
    }
    var spec = {};
    for( var i = 0; i < n; ++i ) {
        if ( Random.rand() > 0.5 ) {
            var bounds = [ r(), r() ];
            if ( bounds[ 0 ] > bounds[ 1 ] ) {
                bounds.reverse();
            }
	    var s = {};
	    if ( Random.rand() > 0.5 ) {
		s[ "$gte" ] = bounds[ 0 ];
	    } else {
		s[ "$gt" ] = bounds[ 0 ];
	    }
	    if ( Random.rand() > 0.5 ) {
		s[ "$lte" ] = bounds[ 1 ];
	    } else {
		s[ "$lt" ] = bounds[ 1 ];
	    }
            spec[ fields[ i ] ] = s;
        } else {
            var vals = []
            for( var j = 0; j < Random.randInt( 15 ); ++j ) {
                vals.push( r() );
            }
            spec[ fields[ i ] ] = { $in: vals };
        }
    }
    s = sort();
    c1 = t.find( spec, { _id:null } ).sort( s ).hint( idx ).toArray();

    // Potentially we could have to much data to do an in memory sort and this would fail.
    c3 = lowPassFilter( t.find( spec, { _id:null } ).sort( s ).hint( {$natural:1} ).toArray() );

    assert.eq( c1, c3 );
}

for( var i = 0; i < 10000; ++i ) {
    t.save( obj() );
}
    
t.ensureIndex( idx );
check();

for( var i = 0; i < 10000; ++i ) {
    if ( Random.rand() > 0.9 ) {
        t.save( obj() );
    } else {
        t.remove( obj() ); // improve
    }
    if( Random.rand() > 0.999 ) {
        print( i );
        check();
    }
}

check();

}

for( var z = 0; z < 5; ++z ) {
    doIt();
}