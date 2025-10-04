// Tests whether the geospatial search is stable under btree updates
//
// Tests the implementation of the 2d search, not the behavior we promise.  MongoDB currently
// promises no isolation, so there is no guarantee that we get the results we expect in this file.
//
// The old query system, if it saw a 2d query, would never consider a collscan.
//
// The new query system can answer the queries in this file with a collscan and ranks
// the collscan against the indexed result.
//
// In order to expose the specific NON GUARANTEED isolation behavior this file tests
// we disable table scans to ensure that the new query system only looks at the 2d
// scan.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setParameter.
//   not_allowed_with_signed_security_token,
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
//   requires_getmore,
//   requires_non_retryable_writes,
// ]

assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: true}));

let status = function (msg) {
    print("\n\n###\n" + msg + "\n###\n\n");
};

let coll = db.getCollection("jstests_geo_update_btree2");
coll.drop();

coll.createIndex({loc: "2d"});

status("Inserting points...");

let numPoints = 10;
Random.setRandomSeed();
for (i = 0; i < numPoints; i++) {
    coll.insert({_id: i, loc: [Random.rand() * 180, Random.rand() * 180], i: i % 2});
}

status("Starting long query...");

let query = coll
    .find({
        loc: {
            $within: {
                $box: [
                    [-180, -180],
                    [180, 180],
                ],
            },
        },
    })
    .batchSize(2);
let firstValues = [query.next()._id, query.next()._id];
printjson(firstValues);

status("Removing points not returned by query...");

let allQuery = coll.find();
let removeIds = [];
while (allQuery.hasNext()) {
    let id = allQuery.next()._id;
    if (firstValues.indexOf(id) < 0) {
        removeIds.push(id);
    }
}

let updateIds = [];
for (var i = 0, max = removeIds.length / 2; i < max; i++) updateIds.push(removeIds.pop());

printjson(removeIds);
coll.remove({_id: {$in: removeIds}});

status("Updating points returned by query...");
printjson(updateIds);

let big = new Array(3000).toString();
for (var i = 0; i < updateIds.length; i++) coll.update({_id: updateIds[i]}, {$set: {data: big}});

status("Counting final points...");

// It's not defined whether or not we return documents that are modified during a query.  We
// shouldn't crash, but it's not defined how many results we get back.  This test is modifying every
// doc not returned by the query, and since we currently handle the invalidation by removing them,
// we won't return them.  But we shouldn't crash.
// assert.eq( ( numPoints - 2 ) / 2, query.itcount() )
query.itcount();

assert.commandWorked(db._adminCommand({setParameter: 1, notablescan: false}));
