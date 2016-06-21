// Integration testing for the plan cache and index filter commands with collation.
(function() {
    'use strict';

    var coll = db.collation_plan_cache;
    coll.drop();

    assert.writeOK(coll.insert({a: 'foo', b: 5}));

    // We need two indexes that each query can use so that a plan cache entry is created.
    assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: 'en_US'}}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}, {collation: {locale: 'en_US'}}));

    // We need an index with a different collation, so that string comparisons affect the query
    // shape.
    assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: 'fr_CA'}}));

    // listQueryShapes().

    // Run a query so that an entry is inserted into the cache.
    assert.commandWorked(
        coll.runCommand("find", {filter: {a: 'foo', b: 5}, collation: {locale: "en_US"}}),
        'find command failed');

    // The query shape should have been added.
    var shapes = coll.getPlanCache().listQueryShapes();
    assert.eq(1, shapes.length, 'unexpected cache size after running query');
    assert.eq(shapes[0],
              {
                query: {a: 'foo', b: 5},
                sort: {},
                projection: {},
                collation: {
                    locale: 'en_US',
                    caseLevel: false,
                    caseFirst: 'off',
                    strength: 3,
                    numericOrdering: false,
                    alternate: 'non-ignorable',
                    maxVariable: 'punct',
                    normalization: false,
                    backwards: false,
                    version: '57.1'
                }
              },
              'unexpected query shape returned from listQueryShapes()');

    coll.getPlanCache().clear();

    // getPlansByQuery().

    // Passing a query with an empty collation object should throw.
    assert.throws(function() {
        coll.getPlanCache().getPlansByQuery(
            {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {}});
    }, [], 'empty collation object should throw');

    // Passing a query with an invalid collation object should throw.
    assert.throws(function() {
        coll.getPlanCache().getPlansByQuery(
            {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {bad: "value"}});
    }, [], 'invalid collation object should throw');

    // Run a query so that an entry is inserted into the cache.
    assert.commandWorked(
        coll.runCommand("find", {filter: {a: 'foo', b: 5}, collation: {locale: "en_US"}}),
        'find command failed');

    // The query should have cached plans.
    assert.lt(
        0,
        coll.getPlanCache()
            .getPlansByQuery(
                {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {locale: 'en_US'}})
            .length,
        'unexpected number of cached plans for query');

    // Test passing the query, sort, projection, and collation to getPlansByQuery() as  separate
    // arguments.
    assert.lt(
        0,
        coll.getPlanCache().getPlansByQuery({a: 'foo', b: 5}, {}, {}, {locale: 'en_US'}).length,
        'unexpected number of cached plans for query');

    // Test passing the query, sort, projection, and collation to getPlansByQuery() as separate
    // arguments.
    assert.eq(0,
              coll.getPlanCache().getPlansByQuery({a: 'foo', b: 5}).length,
              'unexpected number of cached plans for query');

    // A query with a different collation should have no cached plans.
    assert.eq(
        0,
        coll.getPlanCache()
            .getPlansByQuery(
                {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {locale: 'fr_CA'}})
            .length,
        'unexpected number of cached plans for query');

    // A query with different string locations should have no cached plans.
    assert.eq(0,
              coll.getPlanCache()
                  .getPlansByQuery({
                      query: {a: 'foo', b: 'bar'},
                      sort: {},
                      projection: {},
                      collation: {locale: 'en_US'}
                  })
                  .length,
              'unexpected number of cached plans for query');

    coll.getPlanCache().clear();

    // clearPlansByQuery().

    // Passing a query with an empty collation object should throw.
    assert.throws(function() {
        coll.getPlanCache().clearPlansByQuery(
            {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {}});
    }, [], 'empty collation object should throw');

    // Passing a query with an invalid collation object should throw.
    assert.throws(function() {
        coll.getPlanCache().clearPlansByQuery(
            {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {bad: "value"}});
    }, [], 'invalid collation object should throw');

    // Run a query so that an entry is inserted into the cache.
    assert.commandWorked(
        coll.runCommand("find", {filter: {a: 'foo', b: 5}, collation: {locale: "en_US"}}),
        'find command failed');
    assert.eq(1,
              coll.getPlanCache().listQueryShapes().length,
              'unexpected cache size after running query');

    // Dropping a query shape with a different collation should have no effect.
    coll.getPlanCache().clearPlansByQuery(
        {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {locale: 'fr_CA'}});
    assert.eq(1,
              coll.getPlanCache().listQueryShapes().length,
              'unexpected cache size after dropping uncached query shape');

    // Dropping a query shape with different string locations should have no effect.
    coll.getPlanCache().clearPlansByQuery(
        {query: {a: 'foo', b: 'bar'}, sort: {}, projection: {}, collation: {locale: 'en_US'}});
    assert.eq(1,
              coll.getPlanCache().listQueryShapes().length,
              'unexpected cache size after dropping uncached query shape');

    // Dropping query shape.
    coll.getPlanCache().clearPlansByQuery(
        {query: {a: 'foo', b: 5}, sort: {}, projection: {}, collation: {locale: 'en_US'}});
    assert.eq(0,
              coll.getPlanCache().listQueryShapes().length,
              'unexpected cache size after dropping query shapes');

    // Index filter commands.

    // planCacheSetFilter should fail if 'collation' is an empty object.
    assert.commandFailed(
        coll.runCommand('planCacheSetFilter',
                        {query: {a: 'foo', b: 5}, collation: {}, indexes: [{a: 1, b: 1}]}),
        'planCacheSetFilter should fail on empty collation object');

    // planCacheSetFilter should fail if 'collation' is an invalid object.
    assert.commandFailed(
        coll.runCommand(
            'planCacheSetFilter',
            {query: {a: 'foo', b: 5}, collation: {bad: "value"}, indexes: [{a: 1, b: 1}]}),
        'planCacheSetFilter should fail on invalid collation object');

    // Set a plan cache filter.
    assert.commandWorked(
        coll.runCommand(
            'planCacheSetFilter',
            {query: {a: 'foo', b: 5}, collation: {locale: 'en_US'}, indexes: [{a: 1, b: 1}]}),
        'planCacheSetFilter failed');

    // Check the plan cache filter was added.
    var res = coll.runCommand('planCacheListFilters');
    assert.commandWorked(res, 'planCacheListFilters failed');
    assert.eq(1, res.filters.length, 'unexpected number of plan cache filters');
    assert.eq(res.filters[0],
              {
                query: {a: 'foo', b: 5},
                sort: {},
                projection: {},
                collation: {
                    locale: 'en_US',
                    caseLevel: false,
                    caseFirst: 'off',
                    strength: 3,
                    numericOrdering: false,
                    alternate: 'non-ignorable',
                    maxVariable: 'punct',
                    normalization: false,
                    backwards: false,
                    version: '57.1'
                },
                indexes: [{a: 1, b: 1}]
              },
              'unexpected plan cache filter');

    // planCacheClearFilters should fail if 'collation' is an empty object.
    assert.commandFailed(
        coll.runCommand('planCacheClearFilters', {query: {a: 'foo', b: 5}, collation: {}}),
        'planCacheClearFilters should fail on empty collation object');

    // planCacheSetFilter should fail if 'collation' is an invalid object.
    assert.commandFailed(coll.runCommand('planCacheClearFilters',
                                         {query: {a: 'foo', b: 5}, collation: {bad: 'value'}}),
                         'planCacheClearFilters should fail on invalid collation object');

    // Clearing a plan cache filter with no collation should have no effect.
    assert.commandWorked(coll.runCommand('planCacheClearFilters', {query: {a: 'foo', b: 5}}));
    assert.eq(1,
              coll.runCommand('planCacheListFilters').filters.length,
              'unexpected number of plan cache filters');

    // Clearing a plan cache filter with a different collation should have no effect.
    assert.commandWorked(coll.runCommand('planCacheClearFilters',
                                         {query: {a: 'foo', b: 5}, collation: {locale: 'fr_CA'}}));
    assert.eq(1,
              coll.runCommand('planCacheListFilters').filters.length,
              'unexpected number of plan cache filters');

    // Clearing a plan cache filter with different string locations should have no effect.
    assert.commandWorked(coll.runCommand(
        'planCacheClearFilters', {query: {a: 'foo', b: 'bar', collation: {locale: 'en_US'}}}));
    assert.eq(1,
              coll.runCommand('planCacheListFilters').filters.length,
              'unexpected number of plan cache filters');

    // Clear plan cache filter.
    assert.commandWorked(coll.runCommand('planCacheClearFilters',
                                         {query: {a: 'foo', b: 5}, collation: {locale: 'en_US'}}));
    assert.eq(0,
              coll.runCommand('planCacheListFilters').filters.length,
              'unexpected number of plan cache filters');
})();