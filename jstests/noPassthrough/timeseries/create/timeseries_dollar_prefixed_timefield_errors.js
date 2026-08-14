/**
 * Timeseries collections with $-prefixed timeField/metaField were disallowed in 8.3. Verifies
 * that such collections cannot be created, either directly via 'create' or via $out.
 *
 * @tags: [requires_timeseries]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const inputCollTimeName = jsTestName() + "_input_for_dollar_time_out";
const inputCollMetaName = jsTestName() + "_input_for_dollar_meta_out";

const dollarTimeValues = [
    {"$t": ISODate(), m: 1.5, _id: 1},
    {"$t": ISODate(), m: 2.5, _id: 2},
];

const dollarMetaValues = [
    {t: ISODate(), "$m": 3.5, _id: 1},
    {t: ISODate(), "$m": 4.5, _id: 2},
];

function createDollarTimeCollection(db, collName) {
    return db.createCollection(collName, {timeseries: {timeField: "$t"}});
}

function createDollarMetaCollection(db, collName) {
    return db.createCollection(collName, {timeseries: {timeField: "t", metaField: "$m"}});
}

function createOutCollection(db, inputCollName, outputCollName, timeseriesOptions) {
    return db.runCommand({
        aggregate: inputCollName,
        pipeline: [
            {
                $out: {
                    db: db.getName(),
                    coll: outputCollName,
                    timeseries: timeseriesOptions,
                },
            },
        ],
        cursor: {},
    });
}

function createOutDollarTimeCollection(db, collName) {
    return createOutCollection(db, inputCollTimeName, collName, {timeField: "$t"});
}

function createOutDollarMetaCollection(db, collName) {
    return createOutCollection(db, inputCollMetaName, collName, {timeField: "t", metaField: "$m"});
}

describe("$-prefixed timeseries timeField/metaField", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.db = this.conn.getDB(jsTestName());

        // Set up regular input collections for the $out cases.
        const inputCollTime = this.db[inputCollTimeName];
        inputCollTime.drop();
        assert.commandWorked(inputCollTime.insertMany(dollarTimeValues));

        const inputCollMeta = this.db[inputCollMetaName];
        inputCollMeta.drop();
        assert.commandWorked(inputCollMeta.insertMany(dollarMetaValues));
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("rejects a $-prefixed timeField in create", function () {
        assert.commandFailedWithCode(
            createDollarTimeCollection(this.db, jsTestName() + "_failed_1"),
            ErrorCodes.BadValue,
        );
    });

    it("rejects a $-prefixed metaField in create", function () {
        assert.commandFailedWithCode(
            createDollarMetaCollection(this.db, jsTestName() + "_failed_2"),
            ErrorCodes.BadValue,
        );
    });

    it("rejects a $-prefixed timeField via $out", function () {
        assert.commandFailedWithCode(
            createOutDollarTimeCollection(this.db, jsTestName() + "_failed_3"),
            ErrorCodes.BadValue,
        );
    });

    it("rejects a $-prefixed metaField via $out", function () {
        assert.commandFailedWithCode(
            createOutDollarMetaCollection(this.db, jsTestName() + "_failed_4"),
            ErrorCodes.BadValue,
        );
    });

    it("rejects a $-prefixed timeField with a valid metaField", function () {
        assert.commandFailedWithCode(
            this.db.createCollection(jsTestName() + "_failed_5", {
                timeseries: {timeField: "$t", metaField: "m"},
            }),
            ErrorCodes.BadValue,
        );
    });

    it("rejects $-prefixed timeField and metaField together", function () {
        assert.commandFailedWithCode(
            this.db.createCollection(jsTestName() + "_failed_6", {
                timeseries: {timeField: "$t", metaField: "$m"},
            }),
            ErrorCodes.BadValue,
        );
    });
});
