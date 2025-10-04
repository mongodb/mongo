/**
 * indexed_insert_2d.js
 *
 * Inserts documents into an indexed collection and asserts that the documents
 * appear in both a collection scan and an index scan. The indexed value is a
 * legacy coordinate pair, indexed with a 2d index.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.indexedField = "indexed_insert_2d";
    // Remove the shard key for 2d indexes, as they are not supported
    delete $config.data.shardKey;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        assert.lt(this.tid, 1 << 16); // assume tid is a 16 bit nonnegative int
        // split the tid into the odd bits and the even bits
        // for example:
        //  tid:  57 = 00111001
        //  even:      0 1 0 1  = 5
        //  odd:        0 1 1 0 = 6
        // This lets us turn every tid into a unique pair of numbers within the range [0, 255].
        // The pairs are then normalized to have valid longitude and latitude values.
        let oddBits = 0;
        let evenBits = 0;
        for (let i = 0; i < 16; ++i) {
            if (this.tid & (1 << i)) {
                if (i % 2 === 0) {
                    // i is even
                    evenBits |= 1 << (i / 2);
                } else {
                    // i is odd
                    oddBits |= 1 << (i / 2);
                }
            }
        }
        assert.lt(oddBits, 256);
        assert.lt(evenBits, 256);
        this.indexedValue = [(evenBits - 128) / 2, (oddBits - 128) / 2];
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        let ixSpec = {};
        ixSpec[this.indexedField] = "2d";
        return ixSpec;
    };

    return $config;
});
