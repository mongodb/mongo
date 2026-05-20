/**
 * Tests that validate uses the collection's collator when comparing observed min/max against
 * control in time-series buckets. When a measurement is deleted from a v3 (unsorted) bucket, the
 * bucket is rebuilt as v2 (sorted). The rebuild computes control.min/max from the unsorted
 * BSONColumn order, but compressBucket re-sorts the data by time. For collation-equal values with
 * different byte sequences (e.g. "berlin"/"Berlin"), first-seen-wins picks different representatives
 * depending on iteration order, so control and the re-sorted data can diverge in bytes. The
 * collator in woCompare prevents this from being flagged as a mismatch.
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

function setupCollection(collName) {
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {timeField: "t", metaField: "m"},
            collation: {locale: "en", strength: 2},
        }),
    );
    const c = db.getCollection(collName);
    return {coll: c, rawColl: getTimeseriesCollForRawOps(db, c)};
}

// Case 1: min == max ("berlin"/"Berlin").
{
    const {coll, rawColl} = setupCollection("ts1");

    assert.commandWorked(
        coll.insertMany([
            {t: ISODate("2024-01-01T00:00:00Z"), m: "a", notes: "filler"},
            {t: ISODate("2024-01-01T00:00:01Z"), m: "a", notes: "berlin"},
        ]),
    );
    assert.commandWorked(coll.insert({t: ISODate("2024-01-01T00:00:00.500Z"), m: "a", notes: "Berlin"}));

    let buckets = rawColl.find().rawData().toArray();
    assert.eq(1, buckets.length);
    assert.eq(TimeseriesTest.BucketVersion.kCompressedUnsorted, buckets[0].control.version);

    assert.commandWorked(coll.deleteOne({notes: "filler"}));

    buckets = rawColl.find().rawData().toArray();
    assert.eq(1, buckets.length);
    assert.eq(TimeseriesTest.BucketVersion.kCompressedSorted, buckets[0].control.version);

    assert.eq("berlin", buckets[0].control.min.notes);
    // The v3→v2 rebuild re-sorts measurements by time in compressBucket, but control.min/max
    // was computed from the unsorted unpack order. The validator reads the time-sorted BSONColumn,
    // so it sees the other byte variant first.
    const docs = coll.find().toArray();
    assert.eq(2, docs.length);
    assert.eq("Berlin", docs[0].notes);
    assert.eq("berlin", docs[1].notes);

    const res = assert.commandWorked(coll.validate());
    assert(res.valid, "Case 1: validate() should pass. Got: " + tojson(res));
    assert.eq(0, res.nNonCompliantDocuments, tojson(res));
}

// Case 2: min != max ("apple"/"Apple" and "zebra"/"Zebra").
{
    const {coll, rawColl} = setupCollection("ts2");

    assert.commandWorked(
        coll.insertMany([
            {t: ISODate("2024-01-01T00:00:00Z"), m: "a", notes: "filler"},
            {t: ISODate("2024-01-01T00:00:01Z"), m: "a", notes: "apple"},
            {t: ISODate("2024-01-01T00:00:03Z"), m: "a", notes: "zebra"},
        ]),
    );
    assert.commandWorked(coll.insert({t: ISODate("2024-01-01T00:00:00.500Z"), m: "a", notes: "Apple"}));
    assert.commandWorked(coll.insert({t: ISODate("2024-01-01T00:00:02Z"), m: "a", notes: "Zebra"}));

    let buckets = rawColl.find().rawData().toArray();
    assert.eq(1, buckets.length);
    assert.eq(TimeseriesTest.BucketVersion.kCompressedUnsorted, buckets[0].control.version);

    assert.commandWorked(coll.deleteOne({notes: "filler"}));

    buckets = rawColl.find().rawData().toArray();
    assert.eq(1, buckets.length);
    assert.eq(TimeseriesTest.BucketVersion.kCompressedSorted, buckets[0].control.version);

    assert.eq("apple", buckets[0].control.min.notes);
    assert.eq("zebra", buckets[0].control.max.notes);
    // The v3→v2 rebuild re-sorts measurements by time in compressBucket, but control.min/max
    // was computed from the unsorted unpack order. The validator reads the time-sorted BSONColumn,
    // so it sees the other byte variants first.
    const docs = coll.find().toArray();
    assert.eq(4, docs.length);
    assert.eq("Apple", docs[0].notes);
    assert.eq("Zebra", docs[2].notes);

    const res = assert.commandWorked(coll.validate());
    assert(res.valid, "Case 2: validate() should pass. Got: " + tojson(res));
    assert.eq(0, res.nNonCompliantDocuments, tojson(res));
}

MongoRunner.stopMongod(conn);
