// SERVER-11675 Text search integration with aggregation

var server11675 = function() {
    var t = db.server11675;
    t.drop();

    if (typeof(RUNNING_IN_SHARDED_AGG_TEST) != undefined) { // see end of testshard1.js
        db.adminCommand( { shardcollection : t.getFullName(), key : { "_id" : 1 } } );
    }

    t.insert({_id: 1, text: "apple", words: 1});
    t.insert({_id: 2, text: "banana", words: 1});
    t.insert({_id: 3, text: "apple banana", words: 2});
    t.insert({_id: 4, text: "cantaloupe", words: 1});

    t.ensureIndex({text: "text"});

    // query should have subfields query, project, sort, skip and limit. All but query are optional.
    var assertSameAsFind = function(query) {
        var cursor = t.find(query.query);
        var pipeline = [{$match: query.query}];

        if ('project' in query) {
            cursor = t.find(query.query, query.project); // no way to add to constructed cursor
            pipeline.push({$project: query.project});
        }

        if ('sort' in query) {
            cursor = cursor.sort(query.sort);
            pipeline.push({$sort: query.sort});
        }

        if ('skip' in query) {
            cursor = cursor.skip(query.skip);
            pipeline.push({$skip: query.skip});
        }

        if ('limit' in query) {
            cursor = cursor.limit(query.limit);
            pipeline.push({$limit: query.limit});
        }

        var findRes = cursor.toArray();
        var aggRes = t.aggregate(pipeline).toArray();
        assert.docEq(aggRes, findRes);
    }

    assertSameAsFind({query: {}}); // sanity check
    assertSameAsFind({query: {$text:{$search:"apple"}}});
    assertSameAsFind({query: {$and:[{$text:{$search:"apple"}}, {_id:1}]}});
    assertSameAsFind({query: {$text:{$search:"apple"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     });
    assertSameAsFind({query: {$text:{$search:"apple banana"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     });
    assertSameAsFind({query: {$text:{$search:"apple banana"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     ,sort: {score: {$meta: "textScore"}}
                     });
    assertSameAsFind({query: {$text:{$search:"apple banana"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     ,sort: {score: {$meta: "textScore"}}
                     ,limit: 1
                     });
    assertSameAsFind({query: {$text:{$search:"apple banana"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     ,sort: {score: {$meta: "textScore"}}
                     ,skip: 1
                     });
    assertSameAsFind({query: {$text:{$search:"apple banana"}}
                     ,project: {_id:1, score: {$meta: "textScore"}}
                     ,sort: {score: {$meta: "textScore"}}
                     ,skip: 1
                     ,limit: 1
                     });

    // sharded find requires projecting the score to sort, but sharded agg does not.
    var findRes = t.find({$text: {$search: "apple banana"}}, {textScore: {$meta: 'textScore'}})
                   .sort({textScore: {$meta: 'textScore'}})
                   .map(function(obj) {
                       delete obj.textScore; // remove it to match agg output
                       return obj;
                   });
    var res = t.aggregate([{$match: {$text: {$search: 'apple banana'}}}
                          ,{$sort: {textScore: {$meta: 'textScore'}}}
                          ]).toArray();
    assert.eq(res, findRes);

    // Make sure {$meta: 'textScore'} can be used as a sub-expression
    var res = t.aggregate([{$match: {_id:1, $text: {$search: 'apple'}}}
                          ,{$project: {words: 1
                                      ,score: {$meta: 'textScore'}
                                      ,wordsTimesScore: {$multiply: ['$words', {$meta:'textScore'}]}
                                      }}
                          ]).toArray();
    assert.eq(res[0].wordsTimesScore, res[0].words * res[0].score, tojson(res));

    // And can be used in $group
    var res = t.aggregate([{$match: {_id: 1, $text: {$search: 'apple banana'}}}
                          ,{$group: {_id: {$meta: 'textScore'}
                                    ,score: {$first: {$meta: 'textScore'}}
                                    }}
                          ]).toArray();
    assert.eq(res[0]._id, res[0].score, tojson(res));

    // Make sure metadata crosses shard -> merger boundary
    var res = t.aggregate([{$match: {_id:1, $text: {$search: 'apple'}}}
                          ,{$project: {scoreOnShard: {$meta: 'textScore'} }}
                          ,{$limit:1} // force a split. later stages run on merger
                          ,{$project: {scoreOnShard:1
                                      ,scoreOnMerger: {$meta: 'textScore'} }}
                          ]).toArray();
    assert.eq(res[0].scoreOnMerger, res[0].scoreOnShard);
}
server11675();
