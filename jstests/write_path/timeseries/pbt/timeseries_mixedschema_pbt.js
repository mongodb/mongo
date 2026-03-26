/**
 * A property-based test that compares compatible command sequences between timeseries
 * and non-timeseries collections, with mixed-schema extra field streams enabled.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   requires_getmore,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getFcParams, getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const fcParams = getFcParams();
const fcAssertArgs = getFcAssertArgs();

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

function classifyValueType(value) {
    if (value === null) {
        return "null";
    }
    if (value === undefined) {
        return "undefined";
    }
    if (value instanceof Date) {
        return "date";
    }
    if (typeof value === "number") {
        return "double";
    }
    if (typeof value === "string") {
        return "string";
    }
    if (typeof value === "boolean") {
        return "bool";
    }
    if (value instanceof Timestamp) {
        return "timestamp";
    }
    if (value instanceof NumberLong) {
        return "long";
    }
    if (value instanceof NumberDecimal) {
        return "decimal";
    }
    if (value instanceof ObjectId) {
        return "objectId";
    }
    if (typeof BinData !== "undefined" && value instanceof BinData) {
        return "binData";
    }
    if (typeof DBPointer !== "undefined" && value instanceof DBPointer) {
        return "dbPointer";
    }
    if (typeof Code !== "undefined" && value instanceof Code) {
        return "javascript";
    }
    return typeof value;
}

function makeOrderedTypeKey(typeSet) {
    return Array.from(typeSet).sort().join("|");
}

function updateGeneratedFieldStreamStats(accumulatedStats, docs) {
    const fieldTypeSets = {};

    for (const doc of docs) {
        for (const [fieldName, value] of Object.entries(doc)) {
            if (["_id", timeField, metaField].includes(fieldName)) {
                continue;
            }

            fieldTypeSets[fieldName] = fieldTypeSets[fieldName] || new Set();
            fieldTypeSets[fieldName].add(classifyValueType(value));
        }
    }

    for (const typeSet of Object.values(fieldTypeSets)) {
        if (typeSet.size > 1) {
            accumulatedStats.fieldStreamsGenerated.mixedTypeFields += 1;
            const typeKey = makeOrderedTypeKey(typeSet);
            accumulatedStats.insertedTypeCombinations[typeKey] =
                (accumulatedStats.insertedTypeCombinations[typeKey] || 0) + 1;
        } else {
            accumulatedStats.fieldStreamsGenerated.singleTypeFields += 1;
        }
    }

    return accumulatedStats;
}

function updateFinalCollectionFieldTypeStats(accumulatedStats, docs) {
    const fieldTypeSets = {};

    for (const doc of docs) {
        for (const [fieldName, value] of Object.entries(doc)) {
            if (["_id", timeField, metaField].includes(fieldName)) {
                continue;
            }

            fieldTypeSets[fieldName] = fieldTypeSets[fieldName] || new Set();
            fieldTypeSets[fieldName].add(classifyValueType(value));
        }
    }

    for (const typeSet of Object.values(fieldTypeSets)) {
        if (typeSet.size > 1) {
            accumulatedStats.finalCollectionFieldTypes.mixedTypeFields += 1;
        } else {
            accumulatedStats.finalCollectionFieldTypes.singleTypeFields += 1;
        }
    }

    return accumulatedStats;
}

describe("Comparative PBT for mixed-schema timeseries field streams", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;
    let stats;

    const beforeHook = () => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField, metaField}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    };

    beforeEach(function () {
        stats = {
            commands: {},
            fieldStreamsGenerated: {
                mixedTypeFields: 0,
                singleTypeFields: 0,
            },
            insertedTypeCombinations: {},
            finalCollectionFieldTypes: {
                mixedTypeFields: 0,
                singleTypeFields: 0,
            },
        };
    });

    afterEach(function () {
        const percentages = {};

        for (const [statSliceName, statSlice] of Object.entries(stats)) {
            const sum = Object.values(statSlice).reduce((acc, val) => acc + val, 0);
            percentages[statSliceName] = Object.fromEntries(
                Object.entries(statSlice).map(([key, val]) => [key, sum === 0 ? 0 : (100.0 * val) / sum]),
            );
        }

        jsTest.log.info({
            "Arbitrary generation stats for this mixed-schema test run": {
                stats,
                percentages,
            },
        });
    });

    const commandStatsReducer = (accumulatedStats, command) => {
        const commandName = command.cmd.constructor.name;
        accumulatedStats.commands[commandName] = (accumulatedStats.commands[commandName] || 0) + 1;

        if (commandName === "BatchInsertCommand") {
            return updateGeneratedFieldStreamStats(accumulatedStats, command.cmd.docs);
        }

        return accumulatedStats;
    };

    it("keeps tsColl and ctrlColl in sync under mixed-schema field streams", () => {
        const programArb = makeTimeseriesCommandSequenceArb(
            fcParams.minCommands || 1, // minCommands
            fcParams.maxCommands || 30, // maxCommands
            timeField,
            metaField,
            metaValue,
            1, // minFields
            3, // maxFields
            0, // minDocs
            50, // maxDocs
            {
                // options
                mixedSchemaChance: 0.5,
            },
            undefined, //fieldNameArb
            fcParams.replayPath,
        );

        fc.assert(
            fc
                .property(programArb, (cmds) => {
                    const model = makeEmptyModel(ctrlColl, bucketColl);

                    fc.modelRun(() => ({model, real: {tsColl, ctrlColl}}), cmds);
                    assertCollectionsMatch(tsColl, ctrlColl);

                    stats = cmds.commands.reduce(commandStatsReducer, stats);
                    stats = updateFinalCollectionFieldTypeStats(stats, tsColl.find().toArray());
                })
                .beforeEach(beforeHook),
            fcAssertArgs,
        );
    });
});
