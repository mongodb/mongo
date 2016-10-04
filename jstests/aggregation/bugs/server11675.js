// SERVER-11675 Text search integration with aggregation
load('jstests/aggregation/extras/utils.js');

var server11675 = function() {
    var t = db.server11675;
    t.drop();

    if (typeof(RUNNING_IN_SHARDED_AGG_TEST) != 'undefined') {  // see end of testshard1.js
        db.adminCommand({shardcollection: t.getFullName(), key: {"_id": 1}});
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
            cursor = t.find(query.query, query.project);  // no way to add to constructed cursor
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
    };

    assertSameAsFind({query: {}});  // sanity check
    assertSameAsFind({query: {$text: {$search: "apple"}}});
    assertSameAsFind({query: {_id: 1, $text: {$search: "apple"}}});
    assertSameAsFind(
        {query: {$text: {$search: "apple"}}, project: {_id: 1, score: {$meta: "textScore"}}});
    assertSameAsFind({
        query: {$text: {$search: "apple banana"}},
        project: {_id: 1, score: {$meta: "textScore"}}
    });
    assertSameAsFind({
        query: {$text: {$search: "apple banana"}},
        project: {_id: 1, score: {$meta: "textScore"}},
        sort: {score: {$meta: "textScore"}}
    });
    assertSameAsFind({
        query: {$text: {$search: "apple banana"}},
        project: {_id: 1, score: {$meta: "textScore"}},
        sort: {score: {$meta: "textScore"}},
        limit: 1
    });
    assertSameAsFind({
        query: {$text: {$search: "apple banana"}},
        project: {_id: 1, score: {$meta: "textScore"}},
        sort: {score: {$meta: "textScore"}},
        skip: 1
    });
    assertSameAsFind({
        query: {$text: {$search: "apple banana"}},
        project: {_id: 1, score: {$meta: "textScore"}},
        sort: {score: {$meta: "textScore"}},
        skip: 1,
        limit: 1
    });

    // sharded find requires projecting the score to sort, but sharded agg does not.
    var findRes = t.find({$text: {$search: "apple banana"}}, {textScore: {$meta: 'textScore'}})
                      .sort({textScore: {$meta: 'textScore'}})
                      .map(function(obj) {
                          delete obj.textScore;  // remove it to match agg output
                          return obj;
                      });
    var res = t.aggregate([
                   {$match: {$text: {$search: 'apple banana'}}},
                   {$sort: {textScore: {$meta: 'textScore'}}}
               ]).toArray();
    assert.eq(res, findRes);

    // Make sure {$meta: 'textScore'} can be used as a sub-expression
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple'}}},
                   {
                     $project: {
                         words: 1,
                         score: {$meta: 'textScore'},
                         wordsTimesScore: {$multiply: ['$words', {$meta: 'textScore'}]}
                     }
                   }
               ]).toArray();
    assert.eq(res[0].wordsTimesScore, res[0].words * res[0].score, tojson(res));

    // And can be used in $group
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple banana'}}},
                   {$group: {_id: {$meta: 'textScore'}, score: {$first: {$meta: 'textScore'}}}}
               ]).toArray();
    assert.eq(res[0]._id, res[0].score, tojson(res));

    // Make sure metadata crosses shard -> merger boundary
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple'}}},
                   {$project: {scoreOnShard: {$meta: 'textScore'}}},
                   {$limit: 1}  // force a split. later stages run on merger
                   ,
                   {$project: {scoreOnShard: 1, scoreOnMerger: {$meta: 'textScore'}}}
               ]).toArray();
    assert.eq(res[0].scoreOnMerger, res[0].scoreOnShard);
    var score = res[0].scoreOnMerger;  // save for later tests

    // Make sure metadata crosses shard -> merger boundary even if not used on shard
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple'}}},
                   {$limit: 1}  // force a split. later stages run on merger
                   ,
                   {$project: {scoreOnShard: 1, scoreOnMerger: {$meta: 'textScore'}}}
               ]).toArray();
    assert.eq(res[0].scoreOnMerger, score);

    // Make sure metadata works if first $project doesn't use it.
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple'}}},
                   {$project: {_id: 1}},
                   {$project: {_id: 1, score: {$meta: 'textScore'}}}
               ]).toArray();
    assert.eq(res[0].score, score);

    // Make sure the pipeline fails if it tries to reference the text score and it doesn't exist.
    var res = t.runCommand(
        {aggregate: t.getName(), pipeline: [{$project: {_id: 1, score: {$meta: 'textScore'}}}]});
    assert.commandFailed(res);

    // Make sure the metadata is 'missing()' when it doesn't exist because the document changed
    var res = t.aggregate([
                   {$match: {_id: 1, $text: {$search: 'apple banana'}}},
                   {$group: {_id: 1, score: {$first: {$meta: 'textScore'}}}},
                   {$project: {_id: 1, scoreAgain: {$meta: 'textScore'}}},
               ]).toArray();
    assert(!("scoreAgain" in res[0]));

    // Make sure metadata works after a $unwind
    t.insert({_id: 5, text: 'mango', words: [1, 2, 3]});
    var res = t.aggregate([
                   {$match: {$text: {$search: 'mango'}}},
                   {$project: {score: {$meta: "textScore"}, _id: 1, words: 1}},
                   {$unwind: '$words'},
                   {$project: {scoreAgain: {$meta: "textScore"}, score: 1}}
               ]).toArray();
    assert.eq(res[0].scoreAgain, res[0].score);

    // Error checking
    // $match, but wrong position
    assertErrorCode(t, [{$sort: {text: 1}}, {$match: {$text: {$search: 'apple banana'}}}], 17313);

    // wrong $stage, but correct position
    assertErrorCode(t,
                    [{$project: {searchValue: {$text: {$search: 'apple banana'}}}}],
                    ErrorCodes.InvalidPipelineOperator);
    assertErrorCode(t, [{$sort: {$text: {$search: 'apple banana'}}}], 17312);
};
server11675();
