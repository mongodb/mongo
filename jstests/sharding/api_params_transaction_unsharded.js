/**
 * When a client calls a mongos command with API parameters, mongos must forward them to shards.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
'use strict';

load('jstests/sharding/libs/mongos_api_params_util.js');
MongosAPIParametersUtil.runTests({inTransaction: true, shardedCollection: false});
})();
