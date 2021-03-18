/**
 * Tests for resharding collection cloner's aggregation pipeline to ensure that $lookup on
 * config.cache.chunks is pushed down to shards to execute as part of the split pipeline.
 *
 * @tags: [requires_fcv_49]
 */
(function() {
'use strict';

// Create a cluster with 2 shards.
const numShards = 2;
const st = new ShardingTest({shards: numShards});
const db = st.s.getDB(`${jsTest.name()}_db`);

function assertLookupRunsOnShards(explain) {
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(explain.splitPipeline.hasOwnProperty("shardsPart"), tojson(explain));
    assert.eq(
        explain.splitPipeline.shardsPart.filter(stage => stage.hasOwnProperty("$lookup")).length,
        1,
        tojson(explain));
    assert(explain.splitPipeline.hasOwnProperty("mergerPart"), tojson(explain));
    assert.eq([], explain.splitPipeline.mergerPart, tojson(explain));
}

// Test that the explain's shardsPart section includes $lookup stage when executing the resharding
// collection cloning aggregation pipeline.
(function testLookupPushedDownToShards() {
    const coll = db.coll;
    coll.drop();
    // Shards the collection into two parts.
    st.shardColl(coll, {a: "hashed"}, false, false);
    const explain = coll.explain().aggregate([
        {$match: {$expr: {$gte: ['$_id', {$literal: 1}]}}},
        {$sort: {_id: 1}},
        {$replaceWith: {original: '$$ROOT'}},
        {$lookup: {    
            from: {        
                db: 'config',        
                coll: 'cache.chunks.test.system.resharding'
            },    
            let: {sk: [        
                '$original.x',        
                {$toHashedIndexKey: '$original.y'}   
            ]},    
            pipeline: [        
                {$match: {$expr: {$eq: ['$shard', 'shard0']}}},        
                {$match: {$expr: {$let: {            
                    vars: {                
                        min: {$map: {input: {$objectToArray: '$_id'}, in: '$$this.v'}},                
                        max: {$map: {input: {$objectToArray: '$max'}, in: '$$this.v'}}            
                    },            
                    in: {$and: [                
                        {$gte: ['$$sk', '$$min']},                
                        {$cond: {                    
                            if: {$allElementsTrue: [{$map: {                        
                                input: '$$max',                        
                                in: {$eq: [{$type: '$$this'}, 'maxKey']}                    
                            }}]},                    
                            then: {$lte: ['$$sk', '$$max']},                    
                            else: {$lt : ['$$sk', '$$max']}                
                        }}            
                    ]}        
                }}}}    
            ],    
            as: 'intersectingChunk'
        }},
        {$match: {intersectingChunk: {$ne: []}}},
        {$replaceWith: '$original'}
    ]);
    assertLookupRunsOnShards(explain);
})();

st.stop();
})();
