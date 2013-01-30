// $unwind applied to an empty array field drops the field from the source document.  SERVER-6131

t = db.jstests_aggregation_server6131;
t.drop();

function assertAggregationResults( expected, aggregation ) {
    ret = t.aggregate( aggregation );
    assert.eq( expected, ret.result );
}

t.drop();

// An empty array document is dropped.
t.save( { _id:0, a:1, b:[], c:2 } );
assertAggregationResults( [], { $unwind:'$b' } );

// Values from a nonempty array in another document are unwound.
t.save( { _id:1, b:[ 4, 5 ] } );
assertAggregationResults( [ { _id:1, b:4 },
                            { _id:1, b:5 } ],
                         { $unwind:'$b' } );

// Another empty array document is dropped.
t.save( { _id:2, b:[] } );
assertAggregationResults( [ { _id:1, b:4 },
                            { _id:1, b:5 } ],
                         { $unwind:'$b' } );

t.drop();

// A nested empty array document is dropped.
t.save( { _id:0, a:1, b:{ x:10, y:[], z:20 }, c:2 } );
assertAggregationResults( [], { $unwind:'$b.y' } );

t.drop();

// A null value document is dropped.
t.save( { _id:0, a:1, b:null, c:2 } );
assertAggregationResults( [], { $unwind:'$b' } );

t.drop();

// A missing value causes the document to be dropped.
t.save( { _id:0, a:1, c:2 } );
assertAggregationResults( [], { $unwind:'$b' } );

t.drop();

// A missing value in an existing nested object causes the document to be dropped.
t.save( { _id:0, a:1, b:{ d:4 }, c:2 } );
assertAggregationResults( [], { $unwind:'$b.y' } );

t.drop();

// A missing value in a missing nested object causes the document to be dropped.
t.save( { _id:0, a:1, b:10, c:2 } );
assertAggregationResults( [], { $unwind:'$b.y' } );
