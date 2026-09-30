/**
 * Tests that $queryStats records whether a query shape was executed within a multi-document
 * transaction via the "inTransaction" field of the query stats key. Because "inTransaction" is a
 * key component, the same query shape run inside and outside a transaction produces two separate,
 * isolated query stats entries.
 * @tags: [requires_replication, uses_transactions]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStats,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collName = "coll";

// Fetches the query stats entries for "find" commands on the test collection.
function getFindEntries(conn) {
    return getQueryStats(conn, {collName, extraMatch: {"key.queryShape.command": "find"}});
}

describe("$queryStats inTransaction key field", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet(getQueryStatsServerParameters());
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        const coll = this.primary.getDB(dbName)[collName];
        assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}]));
    });

    beforeEach(function () {
        resetQueryStatsStore(this.primary, "1MB");
    });

    after(function () {
        this.rst.stopSet();
    });

    it("omits inTransaction for queries run outside a transaction", function () {
        const coll = this.primary.getDB(dbName)[collName];
        assert.eq(1, coll.find({v: {$eq: 2}}).itcount());

        const inTxn = getFindEntries(this.primary).filter((e) => e.key.inTransaction === true);
        assert.eq([], inTxn, "no in-transaction entry expected for a non-transactional query");
    });

    it("records inTransaction: true and isolates the entry for in-transaction queries", function () {
        // Run the shape outside a transaction so both variants of the entry exist.
        assert.eq(
            1,
            this.primary
                .getDB(dbName)
                [collName].find({v: {$eq: 2}})
                .itcount(),
        );

        const session = this.primary.startSession();
        try {
            const sessionColl = session.getDatabase(dbName)[collName];
            session.startTransaction();
            assert.eq(1, sessionColl.find({v: {$eq: 2}}).itcount());
            assert.commandWorked(session.commitTransaction_forTesting());
        } finally {
            session.endSession();
        }

        const entries = getFindEntries(this.primary);
        // The identical find shape run inside and outside a transaction yields two isolated entries.
        assert.eq(2, entries.length, entries);

        const inTxn = entries.filter((e) => e.key.inTransaction === true);
        const notInTxn = entries.filter((e) => !e.key.hasOwnProperty("inTransaction"));
        assert.eq(1, inTxn.length, entries);
        assert.eq(1, notInTxn.length, entries);
    });
});
