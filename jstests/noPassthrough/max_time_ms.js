/*
 * Tests query/command option $maxTimeMS.

 * Creates a sharded cluster.
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For runCommandOnEachPrimary().

function executeTest(db, isMongos) {
    let cursor;
    let error;

    const admin = db.getSiblingDB("admin");

    function setFailPoint(fpCommand) {
        if (isMongos) {
            FixtureHelpers.runCommandOnEachPrimary({db: admin, cmdObj: fpCommand});
        }

        // Also set it on mongos (or if we're not connected to mongos, the primary).
        assert.commandWorked(admin.runCommand(fpCommand));
    }

    //
    // Simple positive test for query: a ~100 second query with a 100ms time limit should be
    // aborted.
    //
    (function simplePositiveCase() {
        const t = db.max_time_ms_simple_positive;

        assert.commandWorked(t.insert(Array.from({length: 1000}, _ => ({}))));
        cursor = t.find({
            $where: function() {
                sleep(100);
                return true;
            }
        });
        cursor.maxTimeMS(100);
        error = assert.throws(function() {
            cursor.itcount();
        }, [], "expected query to abort due to time limit");
        assert.commandFailedWithCode(error, ErrorCodes.MaxTimeMSExpired);
    })();

    //
    // Simple negative test for query: a ~300ms query with a 10s time limit should not hit the time
    // limit.
    //
    (function simpleNegative() {
        const t = db.max_time_ms_simple_negative;

        assert.commandWorked(t.insert([{}, {}, {}]));
        cursor = t.find({
            $where: function() {
                sleep(100);
                return true;
            }
        });
        cursor.maxTimeMS(10 * 1000);
        assert.doesNotThrow(function() {
            cursor.itcount();
        }, [], "expected query to not hit the time limit");
    })();

    //
    // Simple positive test for getmore:
    // - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
    // - The find() has a 4-second time limit; the first batch should run "instantly", but the
    // second
    //   batch takes ~15 seconds, so the getmore should be aborted.
    //
    (function simplePositiveGetMore() {
        const t = db.max_time_ms_simple_positive_get_more;

        assert.commandWorked(t.insert([{_id: 0}, {_id: 1}, {_id: 2}]));  // fast batch
        assert.commandWorked(t.insert(
            [{_id: 3, slow: true}, {_id: 4, slow: true}, {_id: 5, slow: true}]));  // slow batch
        cursor = t.find({
                      $where: function() {
                          if (this.slow) {
                              sleep(5 * 1000);
                          }
                          return true;
                      }
                  }).sort({_id: 1});
        cursor.batchSize(3);
        cursor.maxTimeMS(4 * 1000);
        assert.doesNotThrow(function() {
            cursor.next();
            cursor.next();
            cursor.next();
        }, [], "expected batch 1 (query) to not hit the time limit");
        error = assert.throws(function() {
            cursor.next();
            cursor.next();
            cursor.next();
        }, [], "expected batch 2 (getmore) to abort due to time limit");
        assert.commandFailedWithCode(error, ErrorCodes.MaxTimeMSExpired);
    })();

    //
    // Simple negative test for getmore:
    // - Issue a find() that returns 2 batches: a fast batch, then a slow batch.
    // - The find() has a 10-second time limit; the first batch should run "instantly", and the
    // second
    //   batch takes only ~2 seconds, so both the query and getmore should not hit the time limit.
    //
    (function simpleNegativeGetMore() {
        const t = db.max_time_ms_simple_negative_get_more;

        assert.commandWorked(t.insert([{_id: 0}, {_id: 1}, {_id: 2}]));              // fast batch
        assert.commandWorked(t.insert([{_id: 3}, {_id: 4}, {_id: 5, slow: true}]));  // slow batch
        cursor = t.find({
                      $where: function() {
                          if (this.slow) {
                              sleep(2 * 1000);
                          }
                          return true;
                      }
                  }).sort({_id: 1});
        cursor.batchSize(3);
        cursor.maxTimeMS(10 * 1000);
        assert.doesNotThrow(function() {
            cursor.next();
            cursor.next();
            cursor.next();
        }, [], "expected batch 1 (query) to not hit the time limit");
        assert.doesNotThrow(function() {
            cursor.next();
            cursor.next();
            cursor.next();
        }, [], "expected batch 2 (getmore) to not hit the time limit");
    })();

    //
    // Many-batch positive test for getmore:
    // - Issue a many-batch find() with a 6-second time limit where the results take 10 seconds to
    //   generate; one of the later getmore ops should be aborted.
    //
    (function manyBatchPositiveGetMore() {
        const t = db.max_time_ms_many_batch_positive_get_more;

        for (let i = 0; i < 5; i++) {
            assert.commandWorked(
                t.insert([{_id: 3 * i}, {_id: (3 * i) + 1}, {_id: (3 * i) + 2, slow: true}]));
        }
        cursor = t.find({
                      $where: function() {
                          if (this.slow) {
                              sleep(2 * 1000);
                          }
                          return true;
                      }
                  }).sort({_id: 1});
        cursor.batchSize(3);
        cursor.maxTimeMS(6 * 1000);
        error = assert.throws(function() {
            cursor.itcount();
        }, [], "expected find() to abort due to time limit");
        assert.commandFailedWithCode(error, ErrorCodes.MaxTimeMSExpired);
    })();

    //
    // Many-batch negative test for getmore:
    // - Issue a many-batch find() with a 20-second time limit where the results take 10 seconds to
    //   generate; the find() should not hit the time limit.
    //
    (function manyBatchNegativeGetMore() {
        const t = db.many_batch_negative_get_more;
        for (var i = 0; i < 5; i++) {
            assert.commandWorked(
                t.insert([{_id: 3 * i}, {_id: (3 * i) + 1}, {_id: (3 * i) + 2, slow: true}]));
        }
        cursor = t.find({
                      $where: function() {
                          if (this.slow) {
                              sleep(2 * 1000);
                          }
                          return true;
                      }
                  }).sort({_id: 1});
        cursor.batchSize(3);
        cursor.maxTimeMS(20 * 1000);
        assert.doesNotThrow(function() {
            // SERVER-40305: Add some additional logging here in case this fails to help us track
            // down why it failed.
            assert.commandWorked(db.adminCommand({setParameter: 1, traceExceptions: 1}));
            cursor.itcount();
            assert.commandWorked(db.adminCommand({setParameter: 1, traceExceptions: 0}));
        }, [], "expected find() to not hit the time limit");
    })();

    //
    // Simple positive test for commands: a ~300ms command with a 100ms time limit should be
    // aborted.
    //
    (function simplePositiveSleepCmd() {
        if (isMongos) {
            // Test-only sleep command is not supported on mongos.
            return;
        }
        assert.commandFailedWithCode(
            db.adminCommand({sleep: 1, millis: 300, maxTimeMS: 100, lock: "none"}),
            ErrorCodes.MaxTimeMSExpired);
    })();

    //
    // Simple negative test for commands: a ~300ms command with a 10s time limit should not hit the
    // time limit.
    //
    (function simpleNegativeSleepCmd() {
        if (isMongos) {
            // Test-only sleep command is not supported on mongos.
            return;
        }
        assert.commandWorked(
            db.adminCommand({sleep: 1, millis: 300, maxTimeMS: 10 * 1000, lock: "none"}));
    })();

    //
    // Tests for input validation.
    //
    (function checkValidation() {
        const t = db.max_time_ms_validation;
        assert.commandWorked(t.insert({}));

        // Verify lower boundary for acceptable input (0 is acceptable, 1 isn't).
        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(0).itcount();
        });
        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(NumberInt(0)).itcount();
        });
        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(NumberLong(0)).itcount();
        });
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: 0}));
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: NumberInt(0)}));
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: NumberLong(0)}));

        assert.throws.automsg(function() {
                         t.find().maxTimeMS(-1).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(NumberInt(-1)).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(NumberLong(-1)).itcount();
                     });
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: -1}), ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: NumberInt(-1)}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: NumberLong(-1)}),
                                     ErrorCodes.BadValue);

        // Verify upper boundary for acceptable input (2^31-1 is acceptable, 2^31 isn't).

        var maxValue = Math.pow(2, 31) - 1;

        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(maxValue).itcount();
        });
        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(NumberInt(maxValue)).itcount();
        });
        assert.doesNotThrow.automsg(function() {
            t.find().maxTimeMS(NumberLong(maxValue)).itcount();
        });
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: maxValue}));
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: NumberInt(maxValue)}));
        assert.commandWorked(db.runCommand({ping: 1, maxTimeMS: NumberLong(maxValue)}));

        assert.throws.automsg(function() {
                         t.find().maxTimeMS(maxValue + 1).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(NumberInt(maxValue + 1)).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(NumberLong(maxValue + 1)).itcount();
                     });
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: maxValue + 1}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: NumberInt(maxValue + 1)}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: NumberLong(maxValue + 1)}),
                                     ErrorCodes.BadValue);

        // Verify invalid values are rejected.
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(0.1).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(-0.1).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS().itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS("").itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS(true).itcount();
                     });
        assert.throws.automsg(function() {
                         t.find().maxTimeMS({}).itcount();
                     });
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: 0.1}), ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: -0.1}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: undefined}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: ""}), ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: true}),
                                     ErrorCodes.BadValue);
        assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: {}}), ErrorCodes.BadValue);

        // Verify that the maxTimeMS command argument can be sent with $query-wrapped commands.
        cursor = t.getDB().$cmd.find({ping: 1, maxTimeMS: 0}).limit(-1);
        cursor._ensureSpecial();
        assert.eq(1, cursor.next().ok);

        // Verify that the server rejects invalid command argument $maxTimeMS.
        cursor = t.getDB().$cmd.find({ping: 1, $maxTimeMS: 0}).limit(-1);
        cursor._ensureSpecial();
        assert.commandFailed(cursor.next());

        // Verify that the $maxTimeMS query option can't be sent with $query-wrapped commands.
        cursor = t.getDB().$cmd.find({ping: 1}).limit(-1).maxTimeMS(0);
        cursor._ensureSpecial();
        assert.commandFailed(cursor.next());
    })();

    //
    // Tests for fail points maxTimeAlwaysTimeOut and maxTimeNeverTimeOut.
    //
    (function checkFailPointsWork() {
        // maxTimeAlwaysTimeOut positive test for command.
        const t = db.max_time_ms_always_time_out_fp;
        try {
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"}));
            assert.commandFailedWithCode(db.runCommand({ping: 1, maxTimeMS: 10 * 1000}),
                                         ErrorCodes.MaxTimeMSExpired);
        } finally {
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"}));
        }

        // maxTimeNeverTimeOut positive test for command. Don't run on mongos because there's no
        // sleep command.
        if (!isMongos) {
            try {
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"}));
                assert.commandWorked(
                    db.adminCommand({sleep: 1, millis: 300, maxTimeMS: 100, lock: "none"}));
            } finally {
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"}));
            }
        }

        // maxTimeAlwaysTimeOut positive test for query.
        try {
            setFailPoint({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"});

            assert.throws(function() {
                t.find().maxTimeMS(10 * 1000).itcount();
            }, [], "expected query to trigger maxTimeAlwaysTimeOut fail point");
        } finally {
            setFailPoint({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"});
        }

        // maxTimeNeverTimeOut positive test for query.
        try {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"});
            const coll = db.max_time_ms_never_time_out_positive_query;
            assert.commandWorked(coll.insert([{}, {}, {}]));
            cursor = coll.find({
                $where: function() {
                    sleep(100);
                    return true;
                }
            });
            cursor.maxTimeMS(100);
            assert.doesNotThrow(function() {
                cursor.itcount();
            }, [], "expected query to trigger maxTimeNeverTimeOut fail point");
        } finally {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"});
        }
    })();

    // maxTimeAlwaysTimeOut positive test for getmore.
    (function alwaysTimeOutPositiveTest() {
        const coll = db.max_time_ms_always_time_out_positive_get_more;
        assert.commandWorked(coll.insert([{}, {}, {}]));
        cursor = coll.find().maxTimeMS(10 * 1000).batchSize(2);
        assert.doesNotThrow.automsg(function() {
            cursor.next();
            cursor.next();
        });
        try {
            setFailPoint({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "alwaysOn"});
            assert.throws(function() {
                cursor.next();
            }, [], "expected getmore to trigger maxTimeAlwaysTimeOut fail point");
        } finally {
            setFailPoint({configureFailPoint: "maxTimeAlwaysTimeOut", mode: "off"});
        }
    })();

    // maxTimeNeverTimeOut positive test for getmore.
    (function neverTimeOutPositiveTest() {
        const coll = db.max_time_ms_never_time_out_fp;
        assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));  // fast batch
        assert.commandWorked(coll.insert(
            [{_id: 3, slow: true}, {_id: 4, slow: true}, {_id: 5, slow: true}]));  // slow batch
        cursor = coll.find({
                         $where: function() {
                             if (this.slow) {
                                 sleep(2 * 1000);
                             }
                             return true;
                         }
                     })
                     .sort({_id: 1});
        cursor.batchSize(3);
        cursor.maxTimeMS(2 * 1000);
        assert.doesNotThrow(function() {
            cursor.next();
            cursor.next();
            cursor.next();
        }, [], "expected batch 1 (query) to not hit the time limit");
        try {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"});

            assert.doesNotThrow(function() {
                cursor.next();
                cursor.next();
                cursor.next();
            }, [], "expected batch 2 (getmore) to trigger maxTimeNeverTimeOut fail point");
        } finally {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"});
        }
    })();

    //
    // Test that maxTimeMS is accepted by commands that have an option whitelist.
    //
    (function testCommandsWithOptionWhitelist() {
        const t = db.max_time_ms_option_whitelist;
        // The namespace must exist for collMod to work.
        assert.commandWorked(t.insert({x: 1}));

        // "aggregate" command.
        assert.commandWorked(
            t.runCommand("aggregate", {pipeline: [], cursor: {}, maxTimeMS: 60 * 1000}));

        // "collMod" command.
        assert.commandWorked(t.runCommand("collMod", {maxTimeMS: 60 * 1000}));

        // "createIndexes" command.
        assert.commandWorked(t.runCommand(
            "createIndexes", {indexes: [{key: {x: 1}, name: "x_1"}], maxTimeMS: 60 * 1000}));
    })();

    //
    // Test count shell helper (see SERVER-13334).
    //
    (function testCountShellHelper() {
        const t = db.max_time_ms_shell_helper;
        try {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "alwaysOn"});

            assert.doesNotThrow(function() {
                t.find({}).maxTimeMS(1).count();
            });
        } finally {
            setFailPoint({configureFailPoint: "maxTimeNeverTimeOut", mode: "off"});
        }
    })();
}

const enableTestCmd = {
    setParameter: "enableTestCommands=1"
};
(function runOnStandalone() {
    const conn = MongoRunner.runMongod(enableTestCmd);
    assert.neq(null, conn, "mongod was unable to start up");
    try {
        executeTest(conn.getDB("test"), false);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

(function runSharded() {
    const enableTestCmd = {setParameter: "enableTestCommands=1"};
    const st = new ShardingTest(
        {shards: 2, other: {shardOptions: enableTestCmd, mongosOptions: enableTestCmd}});
    try {
        executeTest(st.s.getDB("test"), true);
    } finally {
        st.stop();
    }
})();
})();
