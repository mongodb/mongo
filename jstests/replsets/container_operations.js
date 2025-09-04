/**
 * Tests that container insert and delete operations on integer and string keyed containers appear on disk.
 *
 * @tags: [requires_replication, requires_wiredtiger]
 */
import {createWtTable, dumpWtTable, wtExtractRecordsFromDump} from "jstests/disk/libs/wt_file_helper.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const SIGTERM = 15;
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const primaryDB = primary.getDB(jsTestName());
const dbpath = rst.getDbPath(primary);

if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    rst.stopSet();
    quit();
}

// Namespace required by container ops. Unused otherwise, we operate on an unrelated container.
assert.commandWorked(primaryDB.createCollection(collName));

function makeCI(ns, uri, k, v) {
    return {op: "ci", ns, container: uri, o: {k, v}};
}

function makeCD(ns, uri, k) {
    return {op: "cd", ns, container: uri, o: {k}};
}

function restartAndGetDB(dbName) {
    rst.stopSet(SIGTERM, /*forRestart*/ true);
    rst.startSet({}, /*restart*/ true);
    return rst.getPrimary().getDB(dbName);
}

function runApplyOps(db, ops) {
    assert.commandWorked(db.runCommand({applyOps: ops}));
}

function toDict(arr) {
    assert.eq(0, arr.length % 2);
    const out = {};
    for (let i = 0; i < arr.length; i += 2) {
        out[arr[i]] = arr[i + 1];
    }
    return out;
}

const ns = `${primaryDB.getName()}.${collName}`;
const binA = BinData(0, "QQ==");
const binB = BinData(0, "Qg==");

const cases = [
    {
        uri: "index-intkeys",
        cfg: "key_format=q,value_format=u",
        ops: [
            (uri) => makeCI(ns, uri, NumberLong(1), binA),
            (uri) => makeCI(ns, uri, NumberLong(2), binB),
            (uri) => makeCD(ns, uri, NumberLong(1)),
        ],
        expected: {
            2: "B",
        },
    },
    {
        uri: "index-stringkeys",
        cfg: "key_format=u,value_format=u",
        ops: [
            (uri) => makeCI(ns, uri, binA, binA),
            (uri) => makeCI(ns, uri, binB, binB),
            (uri) => makeCD(ns, uri, binA),
        ],
        expected: {
            "B": "B",
        },
    },
];

for (const t of cases) {
    rst.stopSet(SIGTERM, /*forRestart*/ true);
    createWtTable(dbpath, t.uri, t.cfg);

    let db = restartAndGetDB(primaryDB.getName());
    runApplyOps(
        db,
        t.ops.map((f) => f(t.uri)),
    );

    rst.stopSet(SIGTERM, /*forRestart*/ true);
    const lines = dumpWtTable(t.uri, dbpath, "pretty");
    const datalines = wtExtractRecordsFromDump(lines);
    assert.docEq(t.expected, toDict(datalines));
}

rst.stopSet();
