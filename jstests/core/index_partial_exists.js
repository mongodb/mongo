
function runTest() {
    t = db.index_filtered_exists;
    t.drop();

    // data first, then build index

    t.insert( { _id : 3 } )
    t.insert( { _id : 2, a : 2 } )
    t.insert( { _id : 1, a : 1, b : 1 } )

    t.ensureIndex( { a : 1, b : 1 } , { partialFilterExpression : { b : { $exists : 1 } } } )
    assert.eq( 2 , t.getIndexes().length , "A1: correct number of indexes was created" )

    assert.eq( 3 , t.find().sort( { a : 1, b : 1 } ).count() , "A2: sort().count() returns all records" )
    assert.eq( 3 , t.find().sort( { a : 1, b : 1 } ).itcount() , "A3: sort().itcount() returns all records" )
    assert.eq( 1 , t.findOne( { a : 1, b : 1 } )._id, "A4: correct document returned when both a, b used as predicates" )
    assert.eq( 2 , t.findOne( { a : 2 } )._id, "A5: correct document returned when only a used as predicate" )

    assert.eq( "a_1_b_1", t.find( { a:1, b:1 }).explain().queryPlanner.winningPlan.inputStage.indexName, 
               "A6: explain() shows the filtered index is really used" )
    assert.eq( undefined, t.find( { a:1 }).explain().queryPlanner.winningPlan.inputStage, 
               "A7: explain() shows the filtered index is NOT used when b not given" )

    // No longer possible to hint() an index that would return incorrect results. What other way is there to see the contents of the index?
    //assert.eq( 1, t.find().hint( { a : 1, b : 1 } ).itcount(), "A9: check that index only contains matching documents, forcing its use with hint()" )


    t.dropIndex( { a : 1, b : 1 } )
    assert.eq( 1, t.getIndexes().length , "A99: verify that filtered index was dropped" )


    // build index first, then insert data + small variation of above
    t.drop()
    t.ensureIndex( { a : 1, b : 1, c : 1 } , { partialFilterExpression : { a : { $exists : 1 }, b : { $exists : 1 }, c : { $exists : 1 } } } )
    assert.eq( 2 , t.getIndexes().length , "B1: correct number of indexes was created" )

    t.insert( { _id : 4 } )
    t.insert( { _id : 3, a : 3 } )
    t.insert( { _id : 2, a : 2, b : 2 } )
    t.insert( { _id : 1, a : 1, b : 1, c : 1 } )
    t.insert( { _id : 0, a : 0, b : 0, c : 0, d : 0 } )

    assert.eq( 1 , t.find( { a : { $gte : 0, $lte : 2 }, b : 1, c : 1 } ).sort( { a : 1 } ).count() , 
               "B2: find().sort().count() returns correct amount of records" )
    assert.eq( 1 , t.find( { a : { $gte : 0, $lte : 2 }, b : 1, c : 1 } ).sort( { a : 1 } ).count() , 
               "B3: find().sort().itcount() returns correct amount of records" )
    assert.eq( "a_1_b_1_c_1" , t.find( { a : { $gte : 0, $lte : 2 }, b : 1, c : 1 } ).sort( { a : 1 } ).
               explain().queryPlanner.winningPlan.inputStage.indexName, 
               "B4: find(a,b,c).sort(a) uses the index" )


    assert.eq( 0 , t.findOne( { a : 0, b : 0, c : 0, d : 0 } )._id, "B5: correct document returned when all a, b, c, d used as predicates" )
    assert.eq( 1 , t.findOne( { a : 1, b : 1, c : 1 } )._id, "B6: correct document returned when all a, b, c used as predicates" )
    assert.eq( 2 , t.findOne( { a : 2, b : 2 } )._id, "B7: correct document returned when only a, b used as predicates" )
    assert.eq( 2 , t.findOne( { a : 2 } )._id, "B8: correct document returned when only a used as predicate" )

    assert.eq( "a_1_b_1_c_1", t.find( { a : { $gte : 0, $lte : 2 }, b : 1, c : 1 } ).sort( { a : 1 } ).
               explain().queryPlanner.winningPlan.inputStage.indexName, 
               "B9: explain() shows the filtered index is really used" )
    assert.eq( undefined,     t.find( { a:3 }).explain().queryPlanner.winningPlan.inputStage, 
               "B10: explain() shows the filtered index is not used when b,c not given" )

    // No longer possible to hint() an index that would return incorrect results. What other way is there to see the contents of the index?
    //assert.eq( 2, t.find().hint( { a : 1, b : 1 } ).itcount(), "B12: check that index only contains matching documents, forcing its use with hint()" )

    t.dropIndex( { a : 1 , b : 1, c : 1 } )
    assert.eq( 1 , t.getIndexes().length , "B99: verify that filtered index was dropped" )


    // Check that migration possible: simultaneously create the same index both as filtered and unfiltered
    //FIXME: Currently not possible to have 2 indexes with the same keys. If it becomes possible, re-enable test
    /*
      t.drop()
      t.createIndex( { a : 1, b : 1 } )
      t.insert( { _id : 0, a : 0, b : 0, c : 0, d : 0 } )
      t.insert( { _id : 1, a : 1, b : 1, c : 1 } )
      t.insert( { _id : 2, a : 2, b : 2 } )
      t.insert( { _id : 3, a : 3 } )
      t.insert( { _id : 4 } )
      t.createIndex( { a : 1, b : 1 } , { name : "a_1_b_1_filtered", 
      partialFilterExpression : { a : { $exists : 1 }, b : { $exists : 1 }, c : { $exists : 1 } } } )
      assert.eq( 3 , t.getIndexes().length , "C1: correct number of indexes was created" )

      // Verify that queries work also with the 2 near-identical indexes

      assert.eq( 1 , t.find( { a : { $gte : 0, $lte : 2 }, b : 1 } ).sort( { a : 1 } ).count() , 
      "C2: sort().count() returns all records" )
      assert.eq( 1 , t.find( { a : { $gte : 0, $lte : 2 }, b : 1 } ).sort( { a : 1 } ).count() , 
      "C3: sort().itcount() returns all records" )


      assert.eq( 0 , t.findOne( { a : 0, b : 0, c : 0, d : 0 } )._id, "C5: correct document returned when all a, b, c, d used as predicates" )
      assert.eq( 1 , t.findOne( { a : 1, b : 1, c : 1 } )._id, "C6: correct document returned when all a, b, c used as predicates" )
      assert.eq( 2 , t.findOne( { a : 2, b : 2 } )._id, "C7: correct document returned when only a, b used as predicates" )
      assert.eq( 3 , t.findOne( { a : 3 } )._id, "C8: correct document returned when only a used as predicate" )

      t.dropIndex( "a_1_b_1" )
      assert.eq( 2 , t.getIndexes().length , "C99: verify that one of the two indexes was dropped" )
    */


    // Same tests as in the beginning, but the filtered column is the first one
    t.drop()
    t.ensureIndex({ b: 1, a: 1 }, { $partialFilterExpression: { b: { $exists: 1 } } })
    assert.eq( 2 , t.getIndexes().length , "D1: correct number of indexes was created" )

    t.insert( { _id : 4, b : 5 } )
    t.insert( { _id : 3, a : 3 } )
    t.insert( { _id : 2, a : 2, b : 2 } )
    t.insert( { _id : 1, a : 1, b : 1, c : 1 } )
    t.insert( { _id : 0, a : 0, b : 0, c : 0, d : 0 } )

    assert.eq( 1 , t.find( { b : 1 } ).count() , 
               "D2: Simple find().count() returns correct amount of records" )
    assert.eq( 3 , t.find( { b: { $in: [ 1, 2, 5, 123 ] } } ).count() , 
               "D3: $in query on filtered column works" )
    assert.eq( 2 , t.find( { b: { $gt: 1, $lte: 5 } } ).count() , 
               "D3: $gt and $lte on filtered column works" )


    assert.eq( "b_1_a_1", t.find( { b : 1 } ).
               explain().queryPlanner.winningPlan.inputStage.indexName, 
               "D10: explain() shows the filtered index is really used" )
    assert.eq( "b_1_a_1", t.find( { b: { $in: [ 1, 2, 5, 123 ] } } ).
               explain().queryPlanner.winningPlan.inputStage.indexName, 
               "D11: explain() shows the filtered index is really used for $in query" )
    assert.eq( "b_1_a_1", t.find( { b: { $gt: 1, $lte: 5 } } ).
               explain().queryPlanner.winningPlan.inputStage.indexName, 
               "D13: explain() shows the filtered index is really used for $gt, $lte query" )

    t.dropIndex( { b : 1, a : 1 } )
    assert.eq( 1 , t.getIndexes().length , "D99: verify that filtered index was dropped" )



    // Sub documents, Arrays (multikey index) and regular expressions
    t.drop()
    t.ensureIndex({ "b.c.d": 1, "a": 1 }, { $partialFilterExpression: { "b.c.d": { $exists: 1 } }, name : "index1" })
    t.ensureIndex({ "b.e.value": 1, "a": 1 }, { $partialFilterExpression: { "b.e.value": { $exists: 1 } }, name : "index2" })
    assert.eq( 3 , t.getIndexes().length , "E1: correct number of indexes was created" )

    t.insert( 
        {
            "a": 45,
            "b": [
                {
                    "c": {
                        "d": 12
                    },
                    "e": [
                        {
                            "key": "One",
                            "value": "foo"
                        }
                    ]
                },
                {
                    "c": {
                        "d": 45
                    }
                }
            ]
        } )
    t.insert( 
        {
            "a": 5,
            "b": [
                {
                    "c": {
                        "d": 99
                    },
                    "e": [
                        {
                            "key": "One",
                            "value": "foo"
                        }
                    ]
                },
                {
                    "c": {
                        "d": 999
                    }
                }
            ]
        } )
    t.insert( 
        {
            "a": 6,
            "b": [
                {
                    "c": {
                        "d": 99
                    },
                    "e": [
                        {
                            "key": "One",
                            "value": "foo"
                        }
                    ]
                },
                {
                    "c": {
                        "d": 999
                    }
                }
            ]
        } )


    assert.eq( 2 , t.find({ "b.e.value": /^fo/, "a": { $gte: 2, $lte: 10 } }).sort({ "a": 1 }).count() , 
               "E2: Regex query returns correct number of records" )

    ex = t.find({ "b.e.value": /^fo/, "a": { $gte: 2, $lte: 10 } }).sort({ "a": 1 }).
        explain().queryPlanner.winningPlan.inputStage.inputStage;

    assert.eq( "index2", ex.indexName ? ex.indexName : ex.inputStage.indexName,
               "E3: explain() shows the filtered index is really used" )

    t.dropIndex( "index1" )
    t.dropIndex( "index2" )
    assert.eq( 1 , t.getIndexes().length , "E99: verify that filtered index was dropped" )

    t.drop()
}

// Don't run test against mongos.
if (!db.runCommand("isdbgrid").isdbgrid) {
    runTest();
}
