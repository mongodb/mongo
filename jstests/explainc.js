// Test cases for explain()'s nscannedObjects.  SERVER-4161

t = db.jstests_explainc;
t.drop();

t.save( { a:1 } );
t.ensureIndex( { a:1 } );

function assertExplain( expected, explain, checkAllPlans ) {
    for( field in expected ) {
        assert.eq( expected[ field ], explain[ field ], field );
    }
    if ( checkAllPlans && explain.allPlans && explain.allPlans.length == 1 ) {
        for( field in expected ) {
            assert.eq( expected[ field ], explain.allPlans[ 0 ][ field ], field );
        }
    }
    return explain;
}

function assertHintedExplain( expected, cursor ) {
    return assertExplain( expected, cursor.hint( { a:1 } ).explain( true ), true );
}

function assertUnhintedExplain( expected, cursor, checkAllPlans ) {
    return assertExplain( expected, cursor.explain( true ), checkAllPlans );
}

// Standard query.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:1 },
                     t.find( { a:1 } ) );

// Covered index query.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:0 /* no object loaded */ },
                     t.find( { a:1 }, { _id:0, a:1 } ) );

// Covered index query, but matching requires loading document.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:1 },
                     t.find( { a:1, b:null }, { _id:0, a:1 } ) );

// $returnKey query.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:0 },
                     t.find( { a:1 } )._addSpecial( "$returnKey", true ) );

// $returnKey query but matching requires loading document.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:1 },
                     t.find( { a:1, b:null } )._addSpecial( "$returnKey", true ) );

// Skip a result.
assertHintedExplain( { n:0, nscanned:1, nscannedObjects:1 },
                     t.find( { a:1 } ).skip( 1 ) );

// Cursor sorted covered index query.
assertHintedExplain( { n:1, nscanned:1, nscannedObjects:0, scanAndOrder:false },
                     t.find( { a:1 }, { _id:0, a:1 } ).sort( { a:1 } ) );

t.dropIndex( { a:1 } );
t.ensureIndex( { a:1, b:1 } );

// In memory sort covered index query.
assertUnhintedExplain( { n:1, nscanned:1, nscannedObjects:1, scanAndOrder:true },
                       t.find( { a:{ $gt:0 } }, { _id:0, a:1 } ).sort( { b:1 } )
                           .hint( { a:1, b:1 } ) );

// In memory sort $returnKey query.
assertUnhintedExplain( { n:1, nscanned:1, scanAndOrder:true },
                       t.find( { a:{ $gt:0 } } )._addSpecial( "$returnKey", true ).sort( { b:1 } )
                           .hint( { a:1, b:1 } ) );

// In memory sort with skip.
assertUnhintedExplain( { n:0, nscanned:1, nscannedObjects:1 /* The record is still loaded. */ },
                       t.find( { a:{ $gt:0 } } ).sort( { b:1 } ).skip( 1 ).hint( { a:1, b:1 } ),
                       false );

// With a multikey index.
t.drop();
t.ensureIndex( { a:1 } );
t.save( { a:[ 1, 2 ] } );

assertHintedExplain( { n:1, scanAndOrder:false },
                     t.find( { a:{ $gt:0 } }, { _id:0, a:1 } ) );
assertHintedExplain( { n:1, scanAndOrder:true },
                     t.find( { a:{ $gt:0 } }, { _id:0, a:1 } ).sort( { b:1 } ) );

// Dedup matches from multiple query plans.
t.drop();
t.ensureIndex( { a:1, b:1 } );
t.ensureIndex( { b:1, a:1 } );
t.save( { a:1, b:1 } );

// Document matched by three query plans.
assertUnhintedExplain( { n:1, nscanned:1, nscannedObjects:1 },
                       t.find( { a:{ $gt:0 }, b:{ $gt:0 } } ) );

// Document matched by three query plans, with sorting.
assertUnhintedExplain( { n:1, nscanned:1, nscannedObjects:1 },
                       t.find( { a:{ $gt:0 }, b:{ $gt:0 } } ).sort( { c:1 } ) );

// Document matched by three query plans, with a skip.
assertUnhintedExplain( { n:0, nscanned:1, nscannedObjects:1 },
                      t.find( { a:{ $gt:0 }, b:{ $gt:0 } } ).skip( 1 ) );

// Hybrid ordered and unordered plans.

t.drop();
t.ensureIndex( { a:1, b:1 } );
t.ensureIndex( { b:1 } );
for( i = 0; i < 30; ++i ) {
    t.save( { a:i, b:i } );
}

// Ordered plan chosen.
assertUnhintedExplain( { cursor:'BtreeCursor a_1_b_1', n:30, nscanned:30, nscannedObjects:30,
                         scanAndOrder:false },
                       t.find( { b:{ $gte:0 } } ).sort( { a:1 } ) );

// QUERY_MIGRATION:
//
// When we scan an index to provide a sort our covering analysis isn't as good as it could be...
//
// Ordered plan chosen with a covered index.
//assertUnhintedExplain( { cursor:'BtreeCursor a_1_b_1', n:30, nscanned:30, nscannedObjects:0,
                         //scanAndOrder:false },
                       //t.find( { b:{ $gte:0 } }, { _id:0, b:1 } ).sort( { a:1 } ) );

// Ordered plan chosen, with a skip.  Skip is not included in counting nscannedObjects for a single
// plan.
assertUnhintedExplain( { cursor:'BtreeCursor a_1_b_1', n:29, nscanned:30, nscannedObjects:30,
                         scanAndOrder:false },
                       t.find( { b:{ $gte:0 } } ).sort( { a:1 } ).skip( 1 ) );

// Unordered plan chosen.
assertUnhintedExplain( { cursor:'BtreeCursor b_1', n:1, nscanned:1,
                         //nscannedObjects:1, nscannedObjectsAllPlans:2,
                         scanAndOrder:true },
                       t.find( { b:1 } ).sort( { a:1 } ) );

// Unordered plan chosen and projected.
assertUnhintedExplain( { cursor:'BtreeCursor b_1', n:1, nscanned:1, nscannedObjects:1,
                         scanAndOrder:true },
                       t.find( { b:1 }, { _id:0, b:1 } ).sort( { a:1 } ) );

// Unordered plan chosen, with a skip.
// Note that all plans are equally unproductive here, so we can't test which one is picked reliably.
assertUnhintedExplain( { n:0 },
                       t.find( { b:1 }, { _id:0, b:1 } ).sort( { a:1 } ).skip( 1 ) );

// Unordered plan chosen, $returnKey specified.
assertUnhintedExplain( { cursor:'BtreeCursor b_1', n:1, nscanned:1, scanAndOrder:true },
                       t.find( { b:1 }, { _id:0, b:1 } ).sort( { a:1 } )
                           ._addSpecial( "$returnKey", true ) );

// Unordered plan chosen, $returnKey specified, matching requires loading document.
assertUnhintedExplain( { cursor:'BtreeCursor b_1', n:1, nscanned:1, nscannedObjects:1,
                         scanAndOrder:true },
                       t.find( { b:1, c:null }, { _id:0, b:1 } ).sort( { a:1 } )
                           ._addSpecial( "$returnKey", true ) );

t.ensureIndex( { a:1, b:1, c:1 } );

// Documents matched by four query plans.
assertUnhintedExplain( { n:30, nscanned:30, nscannedObjects:30,
                         //nscannedObjectsAllPlans:90 // Not 120 because deduping occurs before
                                                    // loading results.
                       },
                       t.find( { a:{ $gte:0 }, b:{ $gte:0 } } ).sort( { b:1 } ) );

for( i = 30; i < 150; ++i ) {
    t.save( { a:i, b:i } );
}

explain = assertUnhintedExplain( { n:150},
                                 t.find( { $or:[ { a:{ $gte:-1, $lte:200 },
                                                   b:{ $gte:0, $lte:201 } },
                                                 { a:{ $gte:0, $lte:201 },
                                                   b:{ $gte:-1, $lte:200 } } ] },
                                         { _id:0, a:1, b:1 } ).hint( { a:1, b:1 } ) );
printjson(explain);
// Check nscannedObjects for each clause.
assert.eq( 0, explain.clauses[ 0 ].nscannedObjects );
