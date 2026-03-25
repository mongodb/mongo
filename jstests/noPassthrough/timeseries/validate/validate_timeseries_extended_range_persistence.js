/**
 * Tests that validate timeseries collections that have extended range timestamps.
 * These tests additionally require persistence.
 *
 * @tags: [
 * requires_fcv_83,
 * requires_persistence
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("Validation against restored catalog", function () {
    const collNameBase = jsTestName();
    this.ord = 0;

    beforeEach(function () {
        this.ord += 1;
        this.dbpath = MongoRunner.dataPath + jsTestName() + this.ord.toString();
        this.collName = `${collNameBase}.${this.ord}`;
        this.conn = MongoRunner.runMongod({dbpath: this.dbpath, cleanData: true});
        this.db = this.conn.getDB(jsTestName());
        this.db.getCollection(this.collName).drop();
        this.db.createCollection(this.collName, {timeseries: {timeField: "t"}});
        this.coll = this.db.getCollection(this.collName);
    });

    for (let date of [
        ISODate("2026-01-01T00:00:00Z"),
        new Date(-1000),
        new Date(137591838720000),
        ISODate("8000-01-01T00:00:00Z"),
    ]) {
        it(`Correctly sets the extended range flag upon restoration for date=${date}`, function () {
            this.coll.insert({t: date, data: "xyz"});
            MongoRunner.stopMongod(this.conn);

            this.conn = MongoRunner.runMongod({
                dbpath: this.dbpath,
                restart: true,
                cleanData: false,
            });
            this.db = this.conn.getDB(jsTestName());

            const res = this.db[this.collName].validate();

            assert(res.valid, res);
            assert.eq(res.nrecords, 1, res);
            assert.eq(res.nNonCompliantDocuments, 0, res);
            assert.eq(res.warnings.length, 0, res);
            assert.eq(res.errors.length, 0, res);
        });
    }

    afterEach(function () {
        this.db.getCollection(this.collName).drop();
        MongoRunner.stopMongod(this.conn, null, {skipValidation: true});
    });
});
