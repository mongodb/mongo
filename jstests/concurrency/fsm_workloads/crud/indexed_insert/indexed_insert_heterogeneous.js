/**
 * indexed_insert_heterogeneous.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is a different BSON type, depending on the thread's id.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/indexed_insert/indexed_insert_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.indexedField = "indexed_insert_heterogeneous";
    $config.data.shardKey = {};
    $config.data.shardKey[$config.data.indexedField] = 1;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        // prefix str with zeroes to make it have length len
        function pad(len, str) {
            const padding = "0".repeat(len);
            return (padding + str).slice(-len);
        }

        function makeOID(tid) {
            let str = pad(24, tid.toString(16));
            return new ObjectId(str);
        }

        function makeDate(tid) {
            let d = new ISODate("2000-01-01T00:00:00.000Z");
            // setSeconds(n) where n >= 60 will just cause the minutes,
            // hours, etc to increase,
            // so this produces a unique date for each tid
            d.setSeconds(tid);
            return d;
        }

        let choices = [
            this.tid, // int
            this.tid.toString(), // string
            this.tid * 0.0001, // float
            {tid: this.tid}, // subdocument
            makeOID(this.tid), // objectid
            makeDate(this.tid), // date
            new Function("", "return " + this.tid + ";"), // function
        ];

        this.indexedValue = choices[this.tid % choices.length];
    };

    return $config;
});
