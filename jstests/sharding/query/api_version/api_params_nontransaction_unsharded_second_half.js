/**
 * When a client calls a mongos command with API parameters, mongos must forward them to shards.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_scripting,
 *   # TODO SERVER-116053: Add support for mapReduce.
 *   # TODO SERVER-116054: Add support for $where.
 *   mozjs_wasm_unsupported,
 * ]
 */
import {MongosAPIParametersUtil} from "jstests/sharding/libs/mongos_api_params_util.js";

MongosAPIParametersUtil.runTestsSecondHalf({inTransaction: false, shardedCollection: false});
