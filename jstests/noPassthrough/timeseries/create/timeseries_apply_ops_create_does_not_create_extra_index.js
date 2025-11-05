/**
 * Tests that applying an oplog entry to create a time-series collection via the apply_ops command
 * does not also create the default index on time and meta field.
 *
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const dbName = jsTestName();
const collName = "ts";

const rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
const collNameForDDL = getTimeseriesCollForDDLOps(db, collName);

const createCollectionOplogEntry = {
    op: "c",
    ns: "test.$cmd",
    o: {
        create: collNameForDDL,
        clusteredIndex: true,
        timeseries: {
            timeField: "t",
            metaField: "m",
            granularity: "seconds",
            bucketMaxSpanSeconds: 3600,
        },
    },
};

assert.commandWorked(
    db.adminCommand({
        applyOps: [createCollectionOplogEntry],
    }),
);

assert.eq(coll.getIndexes().length, 0);

const oplog = db.getSiblingDB("local").oplog.rs;
const allOplogEntries = oplog.find({}).toArray();

const createOplogEntry = oplog.find({op: "c", "o.create": collNameForDDL}).toArray();
assert.eq(createOplogEntry.length, 1, tojson(allOplogEntries));
const createIndexOplogEntry = oplog.find({op: "c", "o.createIndexes": collNameForDDL}).toArray();
assert.eq(createIndexOplogEntry.length, 0, tojson(allOplogEntries));

rst.stopSet();
