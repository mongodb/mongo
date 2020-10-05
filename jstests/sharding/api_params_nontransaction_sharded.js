/**
 * When a client calls a mongos command with API parameters, mongos must forward them to shards.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_find_command,
 * ]
 */

(function() {
'use strict';

load('jstests/sharding/libs/mongos_api_params_util.js');
MongosAPIParametersUtil.runTests({inTransaction: false, shardedCollection: true});
})();
