'use strict';

/**
 * external_data_source.js
 *
 * Runs multiple aggregations with $_externalDataSources option concurrently.
 */
var $config = (function() {
    var data = (() => {
        Random.setRandomSeed();

        let localCollObjs = [];
        for (let i = 0; i < 100000; ++i) {
            localCollObjs.push({
                _id: i,
                g: Random.randInt(100),  // 100 groups
                str1: "strdata_" + Random.randInt(100000000),
            });
        }

        // Used as documents for a foreign collection.
        let foreignCollObjs = [];
        for (let i = 0; i < 100; ++i) {
            foreignCollObjs.push({
                _id: i,
                desc1: "description_" + Random.randInt(100),
                desc2: "description_" + Random.randInt(100),
            });
        }

        return {
            localCollObjs: localCollObjs,
            foreignCollObjs: foreignCollObjs,
            kDefaultPipePath: "",
            pipeName1: "",
            pipeName2: "",
            vcollName1: "",
            vcollName2: "",
        };
    })();

    var states = (function() {
        const kUrlProtocolFile = "file://";

        function init(db, collName) {
            const hostInfo = assert.commandWorked(db.hostInfo());
            this.kDefaultPipePath = (() => {
                return hostInfo.os.type == "Windows" ? "//./pipe/" : "/tmp/";
            })();

            this.pipeName1 = `concurrency_compute_mode_tid${this.tid}_name1`;
            this.pipeName2 = `concurrency_compute_mode_tid${this.tid}_name2`;
            this.vcollName1 = `vcoll${this.tid}_1`;
            this.vcollName2 = `vcoll${this.tid}_2`;
        }

        function scan(db, collName) {
            jsTestLog(`thread_num ${this.tid} scan_case_start`);

            const pipeName1 = this.pipeName1 + "_scan";
            const pipeName2 = this.pipeName2 + "_scan";

            _writeTestPipeObjects(
                pipeName1, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);
            _writeTestPipeObjects(
                pipeName2, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);

            const expectedRes = this.localCollObjs.concat(this.localCollObjs);

            const resArr = db[this.vcollName1]
                               .aggregate([], {
                                   $_externalDataSources: [{
                                       collName: this.vcollName1,
                                       dataSources: [
                                           {
                                               url: kUrlProtocolFile + pipeName1,
                                               storageType: "pipe",
                                               fileType: "bson"
                                           },
                                           {
                                               url: kUrlProtocolFile + pipeName2,
                                               storageType: "pipe",
                                               fileType: "bson"
                                           }
                                       ]
                                   }]
                               })
                               .toArray();
            assert.eq(resArr.length, expectedRes.length);
            for (let i = 0; i < expectedRes.length; ++i) {
                assert.eq(
                    resArr[i], expectedRes[i], `Unexpected obj = ${tojson(resArr[i])} at ${i}`);
            }

            jsTestLog(`thread_num ${this.tid} scan_case_end`);
        }

        function match(db, collName) {
            jsTestLog(`thread_num ${this.tid} match_case_start`);

            const pipeName1 = this.pipeName1 + "_match";

            _writeTestPipeObjects(
                pipeName1, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);

            const expectedRes = this.localCollObjs.filter(obj => obj.g < 50);

            const resArr = db[this.vcollName1]
                               .aggregate([{$match: {g: {$lt: 50}}}], {
                                   $_externalDataSources: [{
                                       collName: this.vcollName1,
                                       dataSources: [{
                                           url: kUrlProtocolFile + pipeName1,
                                           storageType: "pipe",
                                           fileType: "bson"
                                       }]
                                   }]
                               })
                               .toArray();
            assert.eq(resArr.length, expectedRes.length);
            for (let i = 0; i < expectedRes.length; ++i) {
                assert.eq(
                    resArr[i], expectedRes[i], `Unexpected obj = ${tojson(resArr[i])} at ${i}`);
            }

            jsTestLog(`thread_num ${this.tid} match_case_end`);
        }

        function unionWith(db, collName) {
            jsTestLog(`thread_num ${this.tid} unionWith_case_start`);

            const pipeName1 = this.pipeName1 + "_unionWith";
            const pipeName2 = this.pipeName2 + "_unionWith";

            _writeTestPipeObjects(
                pipeName1, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);
            _writeTestPipeObjects(
                pipeName2, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);

            const expectedRes = this.localCollObjs.concat(this.localCollObjs);

            const resArr = db[this.vcollName1]
                               .aggregate([{$unionWith: this.vcollName2}], {
                                   $_externalDataSources: [
                                       {
                                           collName: this.vcollName1,
                                           dataSources: [{
                                               url: kUrlProtocolFile + pipeName1,
                                               storageType: "pipe",
                                               fileType: "bson"
                                           }]
                                       },
                                       {
                                           collName: this.vcollName2,
                                           dataSources: [{
                                               url: kUrlProtocolFile + pipeName2,
                                               storageType: "pipe",
                                               fileType: "bson"
                                           }]
                                       }
                                   ]
                               })
                               .toArray();
            assert.eq(resArr.length, expectedRes.length);
            for (let i = 0; i < expectedRes.length; ++i) {
                assert.eq(
                    resArr[i], expectedRes[i], `Unexpected obj = ${tojson(resArr[i])} at ${i}`);
            }

            jsTestLog(`thread_num ${this.tid} unionWith_case_end`);
        }

        function group(db, collName) {
            jsTestLog(`thread_num ${this.tid} group_case_start`);

            const pipeName1 = this.pipeName1 + "_group";

            _writeTestPipeObjects(
                pipeName1, this.localCollObjs.length, this.localCollObjs, this.kDefaultPipePath);

            const countPerGroup = [];
            for (let i = 0; i < 100; ++i) {
                countPerGroup[i] = 0;
            }
            this.localCollObjs.forEach(obj => {
                countPerGroup[obj.g]++;
            });
            const expectedRes = [];
            countPerGroup.forEach((cnt, idx) => {
                if (cnt > 0) {
                    expectedRes.push({_id: idx, c: cnt});
                }
            });

            const resArr = db[this.vcollName1]
                               .aggregate([{$group: {_id: "$g", c: {$count: {}}}}], {
                                   $_externalDataSources: [{
                                       collName: this.vcollName1,
                                       dataSources: [{
                                           url: kUrlProtocolFile + pipeName1,
                                           storageType: "pipe",
                                           fileType: "bson"
                                       }]
                                   }]
                               })
                               .toArray();
            assert.sameMembers(resArr, expectedRes);

            jsTestLog(`thread_num ${this.tid} group_case_end`);
        }

        return {
            init: init,
            scan: scan,
            match: match,
            unionWith: unionWith,
            group: group,
        };
    })();

    var transitions = {
        init: {scan: 0.25, match: 0.25, unionWith: 0.25, group: 0.25},
        scan: {scan: 0.25, match: 0.25, unionWith: 0.25, group: 0.25},
        match: {scan: 0.25, match: 0.25, unionWith: 0.25, group: 0.25},
        unionWith: {scan: 0.25, match: 0.25, unionWith: 0.25, group: 0.25},
        group: {scan: 0.25, match: 0.25, unionWith: 0.25, group: 0.25},
    };

    function setup(db, collName, cluster) {
    }

    function teardown(db, collName) {
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        startState: 'init',
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
