/**
 * Some constants given from the reported SERVER-85694. These are used in multiple different tests
 * to ensure we get the correct results in different sharding environments.
 */
export const searchQuery = {
    index: 'drugs',
    facet: {
        operator: {exists: {path: 'id'}},
        facets: {
            manufacturers: {type: 'string', path: 'toplevel.manufacturer_name', numBuckets: 10},
            routes: {type: 'string', path: 'toplevel.route', numBuckets: 10}
        }
    }
};
// Captured from an actual mongot.
export const expectedMergePipeline = [
    {$group: {
        _id: {type: "$type", tag: "$tag", bucket: "$bucket"},
        value: {$sum: "$count"}
    }},
    {$facet: {
        count: [{$match: {"_id.type": {$eq: "count"}}}],
        routes: [
            {$match: {"_id.type": {$eq: "facet"}, "_id.tag": { $eq: "routes" }}},
            {$sort: {value: -1, _id: 1}},
            {$limit: 10}
        ],
        manufacturers: [
            {$match: {"_id.type": {$eq: "facet"}, "_id.tag": {$eq: "manufacturers"}}},
            {$sort: {value: -1, _id: 1}},
            {$limit: 10}
        ]
    }},
    {$replaceWith: { 
        count: {lowerBound: {$first: "$count.value"}},
        facet: { 
            routes: {
                buckets: {
                    $map: {
                        input: "$routes",
                        as: "bucket",
                        in: {_id: "$$bucket._id.bucket", count: "$$bucket.value"}
                    }
                }
            },
            manufacturers: {
                buckets: {
                    $map: {
                        input: "$manufacturers",
                        as: "bucket",
                        in: { _id: "$$bucket._id.bucket", count: "$$bucket.value" }
                    }
                }
            }
        }
    }}
];
export const expectedSearchMeta = {
    "count": {"lowerBound": 1},
    "facet": {
        "routes": {"buckets": [{"_id": "ORAL", "count": 1}]},
        "manufacturers": {"buckets": [{"_id": "Factory", "count": 1}]}
    }
};

const protocolVersion = NumberInt(1);

/**
 * Sets mock responses for the expected 'planShardedSearch' command/response for the given
 * connection.
 * @param {Connection} mongotConn
 */
export function expectPlanShardedSearch({mongotConn, coll}) {
    const cmds = [{
        expectedCommand: {
            planShardedSearch: coll.getName(),
            query: searchQuery,
            $db: coll.getDB().getName(),
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: protocolVersion,
            metaPipeline: expectedMergePipeline,
            sortSpec: {$searchScore: -1},
        }
    }];
    mongotConn.setMockResponses(cmds, 1);
}
