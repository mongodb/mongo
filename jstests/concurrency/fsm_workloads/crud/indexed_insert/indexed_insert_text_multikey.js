/**
 * indexed_insert_text_multikey.js
 *
 * like indexed_insert_text.js but the indexed value is an array of strings
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_text.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);
    };

    $config.data.getRandomTextSnippet = function getRandomTextSnippet() {
        var len = Random.randInt(5) + 1;  // ensure we always add some text, not just empty array
        var textArr = [];
        for (var i = 0; i < len; ++i) {
            textArr.push($super.data.getRandomTextSnippet.call(this, arguments));
        }
        return textArr;
    };

    // SERVER-21291: Reduce the thread count to alleviate PV1 failovers on
    // Windows DEBUG hosts.
    $config.threadCount = 5;

    // Remove the shard key, since it cannot be a multikey index
    delete $config.data.shardKey;

    return $config;
});
