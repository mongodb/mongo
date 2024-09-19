// SERVER-533: Aggregation stage to randomly sample documents.

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, "agg_sample");
coll.drop();

// Should return no results on a collection that doesn't exist. Should not crash.
assert.eq(coll.aggregate([{$sample: {size: 10}}]).toArray(), []);

const nItems = 3;
for (let i = 0; i < nItems; i++) {
    assert.commandWorked(coll.insert({_id: i}));
}

[1,
 nItems,
 nItems + 1]
    .forEach(function(size) {
        const results = coll.aggregate([{$sample: {size: size}}]).toArray();
        assert.eq(results.length, Math.min(size, nItems));
    });

// Multiple $sample stages are allowed.
const results = coll.aggregate([{$sample: {size: nItems}}, {$sample: {size: 1}}]).toArray();
assert.eq(results.length, 1);

// Invalid options.
assertErrorCode(coll, [{$sample: 'string'}], 28745);
assertErrorCode(coll, [{$sample: {size: 'string'}}], 28746);
assertErrorCode(coll, [{$sample: {size: -1}}], 28747);
assertErrorCode(coll, [{$sample: {unknownOpt: true}}], 28748);
assertErrorCode(coll, [{$sample: {/* no size */}}], 28749);

// TODO(SERVER-94154): Remove version check here.
const fcvDoc = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0) {
    // Using a sample of size zero is only disallowed in some newer versions.
    assertErrorCode(coll, [{$sample: {size: 0}}], 28747);
}
