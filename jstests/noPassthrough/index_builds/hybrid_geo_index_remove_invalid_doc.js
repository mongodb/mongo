/**
 * Tests that building geo indexes using the hybrid method handles the unindexing of invalid
 * geo documents.
 *
 * @tags: [
 *   # TODO(SERVER-110846): Index on the secondary is not marked multikey.
 *   primary_driven_index_builds_incompatible,
 *   requires_replication,
 * ]
 */

import {HybridGeoIndexTest, Operation} from "jstests/noPassthrough/libs/index_builds/hybrid_geo_index.js";

const options = {};

const invalidKey = 0;
const validKey = 1;

HybridGeoIndexTest.run(options, options, invalidKey, validKey, Operation.REMOVE);
