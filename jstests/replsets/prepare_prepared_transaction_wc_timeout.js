/**
 * Tests that when preparing a prepared transaction, we wait for writeConcern if the client optime
 * is behind the prepareTimestamp.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWriteConcernRetryabilityTest} from "jstests/libs/write_concern_util.js";

const kNodes = 2;

const replTest = new ReplSetTest({nodes: kNodes});
replTest.startSet({verbose: 1});
replTest.initiate();

const priConn = replTest.getPrimary();
const secConn = replTest.getSecondary();

const lsid = UUID();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    priConn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Insert something into the user collection.
runWriteConcernRetryabilityTest(
    priConn,
    secConn,
    {
        insert: "user",
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(34),
        writeConcern: {w: "majority", wtimeout: 200},
    },
    kNodes,
);

// Since we must wait for writeConcern : majority in order for the prepareTimestamp to be
// committed, this test case will timeout when we stop replication on the secondary.
runWriteConcernRetryabilityTest(
    priConn,
    secConn,
    {
        prepareTransaction: 1,
        lsid: {id: lsid},
        txnNumber: NumberLong(39),
        autocommit: false,
        writeConcern: {w: "majority", wtimeout: 200},
    },
    kNodes,
    "admin",
    function (conn) {
        assert.commandWorked(
            conn.getDB("test").runCommand({
                insert: "user",
                documents: [{_id: 50}, {_id: 70}],
                ordered: false,
                lsid: {id: lsid},
                txnNumber: NumberLong(39),
                readConcern: {level: "snapshot"},
                autocommit: false,
                startTransaction: true,
            }),
        );
    },
);

assert.commandWorked(
    priConn.getDB("admin").runCommand({
        abortTransaction: 1,
        lsid: {id: lsid},
        txnNumber: NumberLong(39),
        autocommit: false,
        writeConcern: {w: "majority"},
    }),
);

replTest.stopSet();
