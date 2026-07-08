import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// The single-document-lookup engine that enriched an updateLookup change event with its post-image.
// The serverStatus key under 'changeStreams.updateLookup' is the value of each constant.
export const UpdateLookupExecutor = Object.freeze({
    kAggregation: "aggregation",
    kExpress: "express",
    kSBE: "sbe",
});

/**
 * combine() numeric value of server status metrics between 'a' and 'b'.
 */
function _combineMetrics(a, b, combine) {
    if (typeof b === "number") {
        return combine(typeof a === "number" ? a : 0, b);
    }

    if (typeof b === "object" && b !== null) {
        // NumberLong / NumberInt override valueOf() to return a primitive JS number.
        if (typeof b.valueOf() === "number") {
            const aVal = a != null && typeof a.valueOf === "function" ? a.valueOf() : 0;
            return combine(aVal, b.valueOf());
        }

        const kNonCombinableTypes = [Timestamp, Date, ObjectId, BinData, NumberDecimal];
        if (!kNonCombinableTypes.some((BsonType) => b instanceof BsonType)) {
            const result = {};
            for (const key of Object.keys(b)) {
                result[key] = _combineMetrics(a != null ? a[key] : undefined, b[key], combine);
            }
            return result;
        }
    }
    return b;
}

// Helper object for retrieving change stream metrics from the 'serverStatus' command's output.
export class ServerStatusMetrics {
    static getCsCursorTotalOpened(db) {
        return this.getCsMetrics(db).cursor.totalOpened;
    }

    static getCsCursorLifespan(db) {
        return this.getCsMetrics(db).cursor.lifespan;
    }

    static getCsCursorOpenTotal(db) {
        return this.getCsMetrics(db).cursor.open.total;
    }

    static getCsCursorOpenPinned(db) {
        return this.getCsMetrics(db).cursor.open.pinned;
    }

    static getSsMetrics(db) {
        return assert.commandWorked(db.adminCommand({serverStatus: 1, metrics: 1})).metrics;
    }

    static getCsMetrics(db) {
        return this.getSsMetrics(db).changeStreams;
    }

    static getCsErrorMetrics(db) {
        return this.getCsMetrics(db).error;
    }

    /**
     * Returns serverStatus metrics summed across every data-bearing node in the fixture (every
     * node of a plain replica set, or every node of each shard for a sharded cluster).
     */
    static getSsMetricsAcrossCluster(db) {
        const responses = FixtureHelpers.runCommandOnAllShards({
            db,
            cmdObj: {serverStatus: 1, metrics: 1},
            primaryNodeOnly: false,
        });
        // Sum the per-node metrics into a single cluster-wide snapshot before diffing.
        return responses.reduce(
            (acc, res) => _combineMetrics(acc, res.metrics, (a, b) => a + b),
            {},
        );
    }

    /**
     * Snapshots getSsMetricsAcrossCluster() before fn(), runs fn(), and returns the delta (after -
     * before), treating a missing before-leaf as 0. Callers don't need to know or care whether
     * they're talking to a replica set or a sharded cluster.
     */
    static withServerStatusMetricsAcrossCluster(db, fn) {
        const before = this.getSsMetricsAcrossCluster(db);
        fn();
        const after = this.getSsMetricsAcrossCluster(db);
        return _combineMetrics(before, after, (beforeVal, afterVal) => afterVal - beforeVal);
    }

    static getCsThroughputMetrics(db) {
        const cursor = this.getCsMetrics(db).cursor;
        return {
            docsReturned: cursor.docsReturned.sum,
            bytesReturned: cursor.bytesReturned.sum,
            batchesReturned: cursor.batchesReturned.sum,
            docsExamined: cursor.docsExamined.sum,
            bytesRead: cursor.bytesRead.sum,
        };
    }
}

// Temporarily overrides a nested TestData field at a dot-notation path with 'newValue',
// saving the original so it can be restored via 'restore()'.
export function TestDataModifyGuard(fieldPath, newValue) {
    this.path = fieldPath.split(".");
    this.pathSwap = function (newValue) {
        return this.path.reduce((obj, part, i) => {
            if (!obj || typeof obj !== "object") {
                throw new Error(
                    `could not traverse path component "${part}" because ${toJsonForLog(obj)} is not an object`,
                );
            }
            if (i === this.path.length - 1) {
                const originalValue = obj[part];
                if (newValue === undefined) {
                    delete obj[part];
                } else {
                    obj[part] = newValue;
                }
                return originalValue;
            }
            if (!obj.hasOwnProperty(part)) {
                throw new Error(`could not find property "${part}" in ${toJsonForLog(obj)}`);
            }
            return obj[part];
        }, TestData);
    };
    this.originalValue = this.pathSwap(newValue);
    this.restore = function () {
        this.pathSwap(this.originalValue);
    };
}
