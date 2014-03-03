// Test script from SERVER-1742

// MongoDB test script for mapreduce with geo query

// setup test collection
db.apples.drop()
db.apples.insert( { "geo" : { "lat" : 32.68331909, "long" : 69.41610718 }, "apples" : 5 } );
db.apples.insert( { "geo" : { "lat" : 35.01860809, "long" : 70.92027283 }, "apples" : 2 } );
db.apples.insert( { "geo" : { "lat" : 31.11639023, "long" : 64.19970703 }, "apples" : 11 } );
db.apples.insert( { "geo" : { "lat" : 32.64500046, "long" : 69.36251068 }, "apples" : 4 } );
db.apples.insert( { "geo" : { "lat" : 33.23638916, "long" : 69.81360626 }, "apples" : 9 } );
db.apples.ensureIndex( { "geo" : "2d" } );

center = [ 32.68, 69.41 ];
radius = 10 / 111; // 10km; 1 arcdegree ~= 111km
geo_query = { geo : { '$within' : { '$center' : [ center, radius ] } } };

// geo query on collection works fine
res = db.apples.find( geo_query );
assert.eq( 2, res.count() );

// map function
m = function() {
	emit( null, { "apples" : this.apples } );
};

// reduce function
r = function(key, values) {
	var total = 0;
	for ( var i = 0; i < values.length; i++ ) {
		total += values[i].apples;
	}
	return { "apples" : total };
};

// mapreduce without geo query works fine
res = db.apples.mapReduce( m, r, { out : { inline : 1 } } );

printjson( res )
total = res.results[0];
assert.eq( 31, total.value.apples );

// mapreduce with regular query works fine too
res = db.apples.mapReduce( m, r, { out : { inline : 1 }, query : { apples : { '$lt' : 9 } } } );
total = res.results[0];
assert.eq( 11, total.value.apples );

// mapreduce with geo query gives error on mongodb version 1.6.2
// uncaught exception: map reduce failed: {
// "assertion" : "manual matcher config not allowed",
// "assertionCode" : 13285,
// "errmsg" : "db assertion failure",
// "ok" : 0 }
res = db.apples.mapReduce( m, r, { out : { inline : 1 }, query : geo_query } );
total = res.results[0];
assert.eq( 9, total.value.apples );
