import {findMatchingLogLine} from "jstests/libs/log.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const paddedIndex = 1000000;
const updateComment = jsTestName() + "-COMMENT";

describe("array padding during update interrupt delinquency", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagRecordDelinquentMetrics: true,
                overdueInterruptCheckIntervalMillis: 1000,
                overdueInterruptCheckSamplingRate: 1.0,
            },
        });
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB(jsTestName());

        if (jsTest.options.inEvergreen && isSlowBuild(db)) {
            jsTest.log.info("Aborting test since it's running on a slow build");
            MongoRunner.stopMongod(conn);
            quit();
        }

        assert.commandWorked(db.coll.insertOne({_id: 1, a: []}));
        assert.commandWorked(db.adminCommand({profile: 0, slowms: -1}));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
            conn = null;
        }
    });

    it("checks for interrupt while padding an array so the update is not overdue", function () {
        assert.commandWorked(
            db.runCommand({
                update: db.coll.getName(),
                updates: [{q: {_id: 1}, u: {$set: {["a." + paddedIndex]: 1}}}],
                comment: updateComment,
            }),
        );

        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        const line = findMatchingLogLine(globalLog.log, {
            msg: "Slow query",
            type: "command",
            comment: updateComment,
        });
        assert.neq(line, null, "slow query log line for the update not found");
        const parsedLine = JSON.parse(line);

        const delinquencyInfo = parsedLine.attr.delinquencyInfo;
        assert(
            !delinquencyInfo || !delinquencyInfo.overdueInterruptChecks,
            "update was overdue on checking interrupts",
            {parsedLine},
        );
        assert.gte(
            parsedLine.attr.numInterruptChecks,
            Math.floor(paddedIndex / 2048),
            "array padding did not check for interrupt periodically",
            {parsedLine},
        );
    });
});
