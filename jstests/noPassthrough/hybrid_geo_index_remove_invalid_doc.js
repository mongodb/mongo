/**
 * Tests that building geo indexes using the hybrid method handles the unindexing of invalid
 * geo documents.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/hybrid_geo_index.js');

const options = {};

const invalidKey = 0;
const validKey = 1;

HybridGeoIndexTest.run(options, options, invalidKey, validKey, Operation.REMOVE);
})();
