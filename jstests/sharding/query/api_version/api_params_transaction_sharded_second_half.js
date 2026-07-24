/**
 * When a client calls a mongos command with API parameters, mongos must forward them to shards.
 *
 * @tags: [
 *   # Imports mongos_api_params_util.js, which runs mapReduce/$where, requiring server-side
 *   # scripting.
 *   requires_scripting,
 *   multiversion_incompatible,
 * ]
 */
import {MongosAPIParametersUtil} from "jstests/sharding/libs/mongos_api_params_util.js";

MongosAPIParametersUtil.runTestsSecondHalf({inTransaction: true, shardedCollection: true});
