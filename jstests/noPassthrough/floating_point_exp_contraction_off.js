/**
 * Validates that the server doesn't use fused multiply-add instructions (-ffp-contract=off).
 *
 * @tags: [
 * multiversion_incompatible,
 * ]
 */

(function() {
'use strict';

const conn = MongoRunner.runMongod();

const coll = conn.getDB('test').getCollection('c');

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
assert.commandWorked(coll.insertOne({
    "loc": {
        "type": "Polygon",
        "coordinates": [[
            [-85.0329458713531, 41.3677690255613],
            [-85.0296092033386, 41.3677690255613],
            [-85.0296092033386, 41.360594065847],
            [-85.0329458713531, 41.360594065847],
            [-85.0329458713531, 41.3677690255613]
        ]]
    }
}));

// Assert that the query returns the document. If the query does not return any result, then
// this is likely because of different rounding due to fused multiply-add instructions on platforms
// that have native support, like arm64.
assert.eq(
    1,
    coll.find({
            "loc": {
                "$near": {
                    "$geometry":
                        {"type": "Point", "coordinates": [-85.031218528747559, 41.364586470348961]},
                    "$maxDistance": 0
                }
            }
        })
        .itcount());

MongoRunner.stopMongod(conn);
}());
