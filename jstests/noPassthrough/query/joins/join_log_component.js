/**
 * Test for the join-specific log component 'query.join': checks that the
 * component is registered, that its verbosity can be set independently via its dotted name, and
 * that join-optimization log lines are emitted with the 'Q_JOIN' component tag.
 *
 * @tags: [requires_fcv_91, requires_sbe]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

describe("query.join log component", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            useLogFiles: true,
            setParameter: {internalEnableJoinOptimization: true},
        });
        assert.neq(null, this.conn, "mongod was unable to start up");
        this.db = this.conn.getDB(jsTestName());

        this.baseColl = this.db.base;
        this.foreignColl = this.db.foreign;
        assert.commandWorked(
            this.baseColl.insertMany([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
            ]),
        );
        assert.commandWorked(
            this.foreignColl.insertMany([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
            ]),
        );
        assert.commandWorked(this.baseColl.createIndex({a: 1}));
        assert.commandWorked(this.foreignColl.createIndex({a: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("is registered under the query component", function () {
        const components = this.db.getLogComponents();
        assert(components.query.hasOwnProperty("join"), "query.join not registered", {components});
        // Unset components report -1 and inherit verbosity from the parent.
        assert.eq(components.query.join.verbosity, -1, "unexpected verbosity", {
            query: components.query,
        });
    });

    it("accepts independent verbosity via its dotted name", function () {
        assert.commandWorked(this.db.setLogLevel(3, "query.join"));
        assert.eq(this.db.getLogComponents().query.join.verbosity, 3);
        // -1 clears the override and restores inheritance from 'query'.
        assert.commandWorked(this.db.setLogLevel(-1, "query.join"));
        assert.eq(this.db.getLogComponents().query.join.verbosity, -1);
    });

    it("emits join optimization logs with the Q_JOIN component", function () {
        assert.commandWorked(this.db.setLogLevel(5, "query.join"));
        this.baseColl
            .aggregate([
                {
                    $lookup: {
                        from: this.foreignColl.getName(),
                        localField: "a",
                        foreignField: "a",
                        as: "joined",
                    },
                },
                {$unwind: "$joined"},
            ])
            .toArray();

        // Log ID 11179802 ("Winning join plan") is emitted on every successful join optimization.
        const globalLog = assert.commandWorked(this.db.adminCommand({getLog: "global"}));
        const line = findMatchingLogLine(globalLog.log, {id: 11179802, c: "Q_JOIN"});
        assert.neq(line, null, "no 'Winning join plan' log line with component Q_JOIN");
    });
});
