// Updates are applied once per location to a document with multiple locations iterated
// consecutively.  This is a descriptive, not a normative test.  SERVER-5885

t = db.jstests_geo_update3;
t.drop();

t.ensureIndex( { loc:'2d' } );
// Save a document with two locations.
t.save( { loc:[ [ 0, 0 ], [ 1, 1 ] ] } );

// The update matches both locations without deduping them.
t.update( { loc:{ $within:{ $center:[ [ 0, 0 ], 2 ], $uniqueDocs:false } } },
         { $inc:{ touchCount:1 } }, false, true );

// The document is updated twice.
assert.eq( 2, db.getLastErrorObj().n );
assert.eq( 2, t.findOne().touchCount );
