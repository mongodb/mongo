/**
 * Mirrors time-series inserts into a regular collection using the following approach:
 * - Measurements are generated with a random number of predetermined field names.
 * - Measurements are inserted using ordered and unordered writes.
 *
 * During teardown we compare that the collections are identical.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 *   requires_timeseries,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = (function() {
    const data = {
        timeFieldName: "time",
        metaFieldName: "meta",
        measurementFieldNames: ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"],
        numDevices: 2,
        maxDocsPerBatch: 10,

        // Used to generate unique _id's for measurements across multiple threads.
        nextIds: {},

        getRegularCollectionName: function() {
            return "regular_mirrored_writes";
        },

        getTimeseriesCollectionName: function() {
            return "timeseries_mirrored_writes";
        },

        generateMeasurement: function() {
            const idPrefix = this.tid.toString();
            let id = undefined;
            if (this.nextIds.hasOwnProperty(idPrefix)) {
                id = ++this.nextIds[idPrefix];
            } else {
                this.nextIds[idPrefix] = 0;
                id = 0;
            }

            let doc = {
                _id: idPrefix + "-" + id.toString(),
                [this.timeFieldName]: new Date(),
                [this.metaFieldName]: Math.floor(Math.random() * this.numDevices),
            };

            const numFields = Math.floor(Math.random() * this.measurementFieldNames.length);
            for (let i = 0; i < numFields; i++) {
                const fieldName = this.measurementFieldNames[i];
                doc[fieldName] = Math.floor(Math.random() * 100);
            }

            return doc;
        },

        insertDocs: function(db, ordered) {
            const docsToInsert = Math.floor(Math.random() * this.maxDocsPerBatch) + 1;
            const docs = [];
            for (let i = 0; i < docsToInsert; i++) {
                docs.push(this.generateMeasurement());
            }

            const regColl = db.getCollection(this.getRegularCollectionName());
            const tsColl = db.getCollection(this.getTimeseriesCollectionName());

            let res = assert.commandWorked(regColl.insertMany(docs, {ordered: ordered}));
            assert.eq(res.insertedIds.length, docsToInsert);

            res = tsColl.insertMany(docs, {ordered: ordered});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, docsToInsert);
        }
    };

    const states = {
        init: function(db, collName) {},

        insertSlow: function(db, collName) {
            this.insertDocs(db, /*ordered=*/ true);
        },

        insertFast: function(db, collName) {
            this.insertDocs(db, /*ordered=*/ false);
        },
    };

    const standardTransition = {
        insertSlow: 0.5,
        insertFast: 0.5,
    };

    const transitions = {
        init: standardTransition,
        insertSlow: standardTransition,
        insertFast: standardTransition,
    };

    function setup(db, collName, cluster) {
        assert.commandWorked(db.createCollection(this.getRegularCollectionName()))
        assert.commandWorked(db.createCollection(this.getTimeseriesCollectionName(), {
            timeseries: {
                timeField: this.timeFieldName,
                metaField: this.metaFieldName,
            }
        }));
    }

    function teardown(db, collName, cluster) {
        // Both collections have the same number of documents.
        assert.eq(db.getCollection(this.getTimeseriesCollectionName()).find().itcount(),
                  db.getCollection(this.getRegularCollectionName()).find().itcount());

        // Iterate over all measurements in the time-series collection and ensure there is an
        // identical one in the mirrored regular collection.
        const cursor = db.getCollection(this.getTimeseriesCollectionName()).find();
        while (cursor.hasNext()) {
            const docToFind = cursor.next();
            assert.eq(1,
                      db.getCollection(this.getRegularCollectionName()).find(docToFind).itcount(),
                      "Could not find measurement " + tojson(docToFind));
        }
    }

    return {
        threadCount: 4,
        iterations: 200,
        states: states,
        data: data,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
