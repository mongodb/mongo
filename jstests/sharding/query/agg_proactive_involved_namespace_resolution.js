/**
 * End-to-end tests for proactive involved-namespace resolution on sharded
 * clusters. These cover the scenarios where mongod resolves the transitive closure of
 * sub-pipeline namespaces up-front and either folds them into a single kickback to mongos (when
 * the top-level is itself a view) or attaches them as additionalResolvedNamespaces on a
 * sentinel-primary ResolvedView (when the top-level is a concrete collection).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_timeseries,
 *   featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("proactive involved-namespace resolution", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2, mongos: 1});
    });

    after(() => {
        this.st.stop();
    });

    let dbCounter = 0;
    const makeDb = (tag) => {
        const dbName = `proactiveInv_${tag}_${dbCounter++}`;
        const db = this.st.s.getDB(dbName);
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        return {db, dbName};
    };

    // Case-insensitive collation used throughout the collation tests.
    const ciCollation = {locale: "en", strength: 2};

    // Creates a view, optionally with a collation. Uses createCollection (which accepts a collation
    // option) when a collation is provided, and the simpler createView otherwise.
    const makeView = (db, name, backing, pipeline = [], collation = null) => {
        if (collation) {
            return assert.commandWorked(
                db.createCollection(name, {viewOn: backing, pipeline, collation}),
            );
        }
        return assert.commandWorked(db.createView(name, backing, pipeline));
    };

    // Asserts that a $lookup result has exactly one outer doc whose join array has exactly one
    // entry with the expected label.
    const assertSingleLookupMatch = (
        result,
        {joinField = "matched", labelValue = "found"} = {},
    ) => {
        assert.eq(result.length, 1, tojson(result));
        assert.eq(result[0][joinField].length, 1, tojson(result[0][joinField]));
        assert.eq(result[0][joinField][0].label, labelValue, tojson(result[0][joinField]));
    };

    // Count view kickbacks observed by mongos during `fn()`.
    //
    // `hangBeforeRetryingAggregateAfterViewKickback` sits in
    // cluster_aggregate.cpp's onViewError handler and fires once per
    // CommandOnShardedViewNotSupportedOnMongod retry. We use it as a counter
    // rather than a hang: passing `sleepFor: 0` in the failpoint data trips an
    // escape hatch in CurOpFailpointHelpers::waitWhileFailPointEnabled that
    // breaks out of the wait loop after a single iteration (see
    // curop_failpoint_helpers.cpp), so mongos doesn't actually block.
    //
    // Each kickback bumps the failpoint's `timesEntered` by exactly 2: one hit
    // from the outer `executeIf` entry and one from the `shouldFail()` check
    // at the top of the (immediately-broken) wait loop. We divide by 2 to
    // recover the kickback count.
    const countKickbacks = (fn) => {
        const fpName = "hangBeforeRetryingAggregateAfterViewKickback";
        const fp = configureFailPoint(this.st.s, fpName, {sleepFor: 0});
        try {
            fn();
        } finally {
            const res = assert.commandWorked(
                this.st.s.adminCommand({configureFailPoint: fpName, mode: "off"}),
            );
            fp._finalCount = res.count;
        }
        const entries = fp._finalCount - fp.timesEntered;
        assert.eq(
            entries % 2,
            0,
            `unexpected failpoint entry count ${entries} (expected multiple of 2)`,
        );
        return entries / 2;
    };

    it("rejects a cycle between two views safely", () => {
        const {db} = makeDb("cycle");

        assert.commandWorked(db.runCommand({create: "v1", viewOn: "v2", pipeline: [{$match: {}}]}));

        const createRes = db.runCommand({create: "v2", viewOn: "v1", pipeline: [{$match: {}}]});
        if (!createRes.ok) {
            assert.commandFailedWithCode(
                createRes,
                [ErrorCodes.GraphContainsCycle, ErrorCodes.ViewDepthLimitExceeded],
                "Expected cycle creation to be rejected: " + tojson(createRes),
            );
        } else {
            assert.commandFailedWithCode(
                db.runCommand({aggregate: "v1", pipeline: [], cursor: {}}),
                [ErrorCodes.ViewDepthLimitExceeded, ErrorCodes.GraphContainsCycle],
            );
        }
    });

    it("preserves timeseries metadata when $lookup foreign is a timeseries view", () => {
        const {db} = makeDb("ts");

        assert.commandWorked(
            db.createCollection("ts", {timeseries: {timeField: "t", metaField: "m"}}),
        );
        assert.commandWorked(
            db.ts.insert([
                {t: new Date(), m: "a", v: 1},
                {t: new Date(), m: "b", v: 2},
            ]),
        );
        assert.commandWorked(db.driver.insert([{_id: 1, mKey: "a"}]));

        const r = db.driver
            .aggregate([
                {$lookup: {from: "ts", localField: "mKey", foreignField: "m", as: "tsJoin"}},
            ])
            .toArray();

        assert.eq(r.length, 1, tojson(r));
        assert.eq(r[0].tsJoin.length, 1, tojson(r[0].tsJoin));
        assert.eq(r[0].tsJoin[0].v, 1, tojson(r[0].tsJoin[0]));
        assert.eq(r[0].tsJoin[0].m, "a", tojson(r[0].tsJoin[0]));
    });

    it("takes a single kickback for a pipeline that transitively references multiple views", () => {
        const {db} = makeDb("nested");

        assert.commandWorked(
            db.coll.insert([
                {x: 1, y: 2},
                {x: 1, y: 3},
                {x: 0, y: 2},
            ]),
        );
        makeView(db, "v1", "coll", [{$match: {x: 1}}]);
        makeView(db, "v2", "coll", [{$match: {y: 2}}]);

        let result;
        const kickbacks = countKickbacks(() => {
            result = db.v1.aggregate([{$unionWith: {coll: "v2"}}]).toArray();
        });
        assert.eq(result.length, 4, tojson(result));
        assert.lte(kickbacks, 1, `Expected <=1 kickback, got ${kickbacks}`);
    });

    it("round-trips $graphLookup when foreign is a view", () => {
        const {db} = makeDb("graphLookup");

        assert.commandWorked(
            db.employees.insert([
                {_id: 1, name: "A", reportsTo: null, active: true},
                {_id: 2, name: "B", reportsTo: 1, active: true},
                {_id: 3, name: "C", reportsTo: 2, active: true},
                {_id: 4, name: "D", reportsTo: 2, active: false},
                {_id: 5, name: "E", reportsTo: 1, active: false},
            ]),
        );
        makeView(db, "activeEmployees", "employees", [{$match: {active: true}}]);
        assert.commandWorked(db.driver.insert([{_id: "root", managerId: 3}]));

        const r = db.driver
            .aggregate([
                {
                    $graphLookup: {
                        from: "activeEmployees",
                        startWith: "$managerId",
                        connectFromField: "reportsTo",
                        connectToField: "_id",
                        as: "chain",
                    },
                },
            ])
            .toArray();

        assert.eq(r.length, 1, tojson(r));
        const chainIds = r[0].chain.map((d) => d._id).sort();
        assert.eq(chainIds, [1, 2, 3], tojson(r[0].chain));
    });

    it("folds top-level view and sub-pipeline view into a single kickback", () => {
        const {db} = makeDb("topAndSub");

        assert.commandWorked(
            db.coll.insert([
                {_id: 1, k: "a", v: 10, active: true},
                {_id: 2, k: "b", v: 20, active: true},
                {_id: 3, k: "c", v: 30, active: false},
            ]),
        );
        assert.commandWorked(
            db.lookupColl.insert([
                {_id: 1, k: "a", label: "alpha", published: true},
                {_id: 2, k: "b", label: "beta", published: false},
            ]),
        );
        makeView(db, "topView", "coll", [{$match: {active: true}}]);
        makeView(db, "lookupView", "lookupColl", [{$match: {published: true}}]);

        let r;
        // Without folding, a top-level-view + sub-pipeline-view query takes 2 kickbacks: sentinel
        // sub-pipeline kickback, then a separate top-level kickback. With folding we expect at
        // most one reparse.
        const kickbacks = countKickbacks(() => {
            r = db.topView
                .aggregate([
                    {
                        $lookup: {
                            from: "lookupView",
                            localField: "k",
                            foreignField: "k",
                            as: "joined",
                        },
                    },
                ])
                .toArray();
        });

        r.sort((a, b) => a._id - b._id);
        assert.eq(r.length, 2, tojson(r));
        assert.eq(r[0].k, "a", tojson(r));
        assert.eq(r[0].joined.length, 1, tojson(r[0].joined));
        assert.eq(r[0].joined[0].label, "alpha", tojson(r[0].joined));
        assert.eq(r[1].k, "b", tojson(r));
        assert.eq(r[1].joined.length, 0, tojson(r[1].joined));

        assert.lte(kickbacks, 1, `Expected <=1 kickback (single folded reparse), got ${kickbacks}`);
    });

    it("resolves a transitively-nested foreign view in one kickback", () => {
        const {db} = makeDb("transitive");

        assert.commandWorked(
            db.coll.insert([
                {_id: 1, k: "a", active: true},
                {_id: 2, k: "b", active: false},
            ]),
        );
        assert.commandWorked(db.tags.insert([{_id: 1, k: "a", tag: "vip"}]));
        assert.commandWorked(db.driver.insert([{_id: "d", k: "a"}]));

        makeView(db, "innerView", "tags", [{$match: {tag: "vip"}}]);
        makeView(db, "midView", "coll", [
            {$match: {active: true}},
            {$lookup: {from: "innerView", localField: "k", foreignField: "k", as: "tags"}},
        ]);

        let result;
        const kickbacks = countKickbacks(() => {
            result = db.driver
                .aggregate([
                    {$lookup: {from: "midView", localField: "k", foreignField: "k", as: "joined"}},
                ])
                .toArray();
        });

        assert.eq(
            kickbacks,
            1,
            `Expected 1 kickback (transitive closure resolved upfront), got ${kickbacks}`,
        );
        assert.eq(result.length, 1, tojson(result));
        assert.eq(result[0].joined.length, 1, tojson(result[0].joined));
        assert.eq(result[0].joined[0].tags.length, 1, tojson(result[0].joined[0].tags));
        assert.eq(result[0].joined[0].tags[0].tag, "vip", tojson(result[0].joined[0].tags));
    });

    it("resolves two distinct foreign views in a single pipeline with one kickback", () => {
        const {db} = makeDb("twoForeign");

        assert.commandWorked(db.orders.insert([{_id: 1, cust: "a", prod: "x"}]));
        assert.commandWorked(
            db.customers.insert([
                {_id: 1, cust: "a", active: true},
                {_id: 2, cust: "b", active: false},
            ]),
        );
        assert.commandWorked(
            db.products.insert([
                {_id: 1, prod: "x", inStock: true},
                {_id: 2, prod: "y", inStock: false},
            ]),
        );

        makeView(db, "customersView", "customers", [{$match: {active: true}}]);
        makeView(db, "productsView", "products", [{$match: {inStock: true}}]);

        let result;
        const kickbacks = countKickbacks(() => {
            result = db.orders
                .aggregate([
                    {
                        $lookup: {
                            from: "customersView",
                            localField: "cust",
                            foreignField: "cust",
                            as: "c",
                        },
                    },
                    {
                        $lookup: {
                            from: "productsView",
                            localField: "prod",
                            foreignField: "prod",
                            as: "p",
                        },
                    },
                ])
                .toArray();
        });

        assert.eq(kickbacks, 1, `Expected 1 kickback for two foreign views, got ${kickbacks}`);
        assert.eq(result.length, 1, tojson(result));
        assert.eq(result[0].c.length, 1, tojson(result[0].c));
        assert.eq(result[0].c[0].cust, "a", tojson(result[0].c));
        assert.eq(result[0].p.length, 1, tojson(result[0].p));
        assert.eq(result[0].p[0].prod, "x", tojson(result[0].p));
    });

    it("uses a foreign view's collation when it matches the operation collation", () => {
        const {db} = makeDb("collationMatch");

        // Outer collection and foreign view share the same case-insensitive collation.
        assert.commandWorked(db.createCollection("orders", {collation: ciCollation}));
        assert.commandWorked(db.orders.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.tags.insert([{_id: 1, name: "alice", label: "found"}]));
        makeView(db, "tagsView", "tags", [], ciCollation);

        // "Alice" matches the stored "alice" under the shared case-insensitive collation.
        const result = db.orders
            .aggregate([
                {
                    $lookup: {
                        from: "tagsView",
                        localField: "tag",
                        foreignField: "name",
                        as: "matched",
                    },
                },
            ])
            .toArray();

        assertSingleLookupMatch(result);
    });

    it("allows $lookup when the explicit request collation matches the foreign view collation (top-level has simple collation)", () => {
        // The top-level collection has no collation (simple/default). The foreign view has a
        // case-insensitive collation and a $match that only finds its row under case-insensitive
        // ("ALICE" != "alice" under simple, == under case-insensitive). The operation provides an
        // explicit collation that matches the view's collation, which is the override-allowed path.
        const {db} = makeDb("collationSuccessOverride");

        assert.commandWorked(db.outerColl.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.foreignColl.insert([{_id: 1, name: "alice", label: "found"}]));
        // The view's $match uses "ALICE" — only matches under the view's case-insensitive collation.
        makeView(db, "foreignView", "foreignColl", [{$match: {name: "ALICE"}}], ciCollation);

        const result = db.outerColl
            .aggregate(
                [
                    {
                        $lookup: {
                            from: "foreignView",
                            localField: "tag",
                            foreignField: "name",
                            as: "matched",
                        },
                    },
                ],
                {
                    collation: ciCollation,
                },
            )
            .toArray();

        assertSingleLookupMatch(result);
    });

    it("allows $lookup when the top-level view and foreign view share the same collation with no explicit request collation", () => {
        const {db} = makeDb("collationSuccessShared");

        assert.commandWorked(db.coll.insert([{_id: 1, tag: "Alice", active: true}]));
        assert.commandWorked(db.foreignColl.insert([{_id: 1, name: "alice", label: "found"}]));
        makeView(db, "topView", "coll", [{$match: {active: true}}], ciCollation);
        makeView(db, "foreignView", "foreignColl", [], ciCollation);

        // No explicit request collation — the top-level view's collation is inherited and matches
        // the foreign view's collation.
        const result = db.topView
            .aggregate([
                {
                    $lookup: {
                        from: "foreignView",
                        localField: "tag",
                        foreignField: "name",
                        as: "matched",
                    },
                },
            ])
            .toArray();

        assertSingleLookupMatch(result);
    });

    it("allows $graphLookup when the explicit request collation matches the foreign view collation", () => {
        const {db} = makeDb("collationSuccessGraphLookup");

        // Outer collection has no collation. Nodes are identified by lowercase ids; the foreign
        // view's case-insensitive $match gates which nodes appear in the graph. An explicit
        // case-insensitive request collation matches the view's collation, so this is allowed.
        assert.commandWorked(
            db.nodes.insert([
                {_id: "a", parent: null, active: "YES"},
                {_id: "b", parent: "a", active: "yes"},
                {_id: "c", parent: "a", active: "no"},
            ]),
        );
        makeView(db, "activeNodes", "nodes", [{$match: {active: "YES"}}], ciCollation);
        assert.commandWorked(db.driver.insert([{_id: 1, startId: "b"}]));

        const result = db.driver
            .aggregate(
                [
                    {
                        $graphLookup: {
                            from: "activeNodes",
                            startWith: "$startId",
                            connectFromField: "parent",
                            connectToField: "_id",
                            as: "ancestors",
                        },
                    },
                ],
                {collation: ciCollation},
            )
            .toArray();

        assert.eq(result.length, 1, tojson(result));
        // "b" is active ("yes" == "YES" under CI), so it is found, and its parent "a" is also
        // found (active: "YES"). "c" does not appear because it is not a reachable ancestor of "b".
        const ancestorIds = result[0].ancestors.map((d) => d._id).sort();
        assert.eq(ancestorIds, ["a", "b"], tojson(result[0].ancestors));
    });

    it("allows $graphLookup when the top-level view and foreign view share the same collation", () => {
        const {db} = makeDb("collationSuccessGraphLookupShared");

        assert.commandWorked(
            db.nodes.insert([
                {_id: "a", parent: null},
                {_id: "b", parent: "a"},
            ]),
        );
        makeView(db, "nodesTopView", "nodes", [], ciCollation);
        makeView(db, "nodesForeignView", "nodes", [], ciCollation);

        const result = db.nodesTopView
            .aggregate([
                {$match: {_id: "b"}},
                {
                    $graphLookup: {
                        from: "nodesForeignView",
                        startWith: "$parent",
                        connectFromField: "parent",
                        connectToField: "_id",
                        as: "ancestors",
                    },
                },
            ])
            .toArray();

        assert.eq(result.length, 1, tojson(result));
        assert.eq(result[0].ancestors.map((d) => d._id).sort(), ["a"], tojson(result[0].ancestors));
    });

    it("allows $unionWith when the top-level view and foreign view share the same collation", () => {
        const {db} = makeDb("collationSuccessUnionWith");

        assert.commandWorked(db.base.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.other.insert([{_id: 2, tag: "alice"}]));

        makeView(db, "baseView", "base", [], ciCollation);
        makeView(db, "otherView", "other", [], ciCollation);

        const result = db.baseView.aggregate([{$unionWith: "otherView"}]).toArray();
        assert.eq(result.length, 2, tojson(result));
    });

    it("distinct succeeds on a view with no collation complications", () => {
        const {db} = makeDb("distinctOnView");

        assert.commandWorked(
            db.coll.insert([
                {_id: 1, tag: "a"},
                {_id: 2, tag: "b"},
                {_id: 3, tag: "a"},
            ]),
        );
        makeView(db, "tagView", "coll", [{$match: {tag: {$exists: true}}}]);

        const result = assert.commandWorked(db.runCommand({distinct: "tagView", key: "tag"}));
        assert.sameMembers(result.values, ["a", "b"], tojson(result));
    });

    it("rejects a $lookup against a foreign view whose collation conflicts with the operation", () => {
        const {db} = makeDb("collationConflictLookup");

        assert.commandWorked(db.orders.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.tags.insert([{_id: 1, name: "alice"}]));
        makeView(db, "tagsView", "tags", [], ciCollation);

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "orders",
                pipeline: [
                    {
                        $lookup: {
                            from: "tagsView",
                            localField: "tag",
                            foreignField: "name",
                            as: "matched",
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });

    it("rejects a $unionWith against a foreign view whose collation conflicts with the operation", () => {
        const {db} = makeDb("collationConflictUnionWith");

        assert.commandWorked(db.base.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.other.insert([{_id: 1, tag: "alice"}]));
        makeView(db, "otherView", "other", [], ciCollation);

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "base",
                pipeline: [{$unionWith: {coll: "otherView", pipeline: [{$match: {tag: "ALICE"}}]}}],
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });

    it("rejects a $graphLookup against a foreign view whose collation conflicts with the operation", () => {
        const {db} = makeDb("collationConflictGraphLookup");

        assert.commandWorked(
            db.nodes.insert([
                {_id: "a", parent: null},
                {_id: "b", parent: "A"},
            ]),
        );
        makeView(db, "nodesView", "nodes", [], ciCollation);

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "nodes",
                pipeline: [
                    {$match: {_id: "b"}},
                    {
                        $graphLookup: {
                            from: "nodesView",
                            startWith: "$parent",
                            connectFromField: "parent",
                            connectToField: "_id",
                            as: "ancestors",
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });

    it("rejects an explicit request collation that would override a foreign view's collation", () => {
        const {db} = makeDb("collationOverride");

        assert.commandWorked(db.orders.insert([{_id: 1, tag: "Alice"}]));
        assert.commandWorked(db.tags.insert([{_id: 1, name: "alice"}]));

        makeView(db, "tagsView", "tags", [], ciCollation);

        // An explicit request collation that differs from the view's collation must be rejected.
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "orders",
                pipeline: [
                    {
                        $lookup: {
                            from: "tagsView",
                            localField: "tag",
                            foreignField: "name",
                            as: "matched",
                        },
                    },
                ],
                collation: {locale: "fr"},
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });

    it("rejects a $lookup between a top-level view and a foreign view with a different collation", () => {
        const {db} = makeDb("collationMultiViewLookup");

        assert.commandWorked(db.coll.insert([{_id: 1, k: "a"}]));
        assert.commandWorked(db.foreign.insert([{_id: 1, k: "A"}]));

        // Top-level view: case-insensitive collation.
        makeView(db, "collView", "coll", [], ciCollation);
        // Foreign view: simple (different) collation.
        makeView(db, "foreignView", "foreign", []);

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "collView",
                pipeline: [
                    {
                        $lookup: {
                            from: "foreignView",
                            localField: "k",
                            foreignField: "k",
                            as: "joined",
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });

    it("rejects a $graphLookup between a top-level view and a foreign view with a different collation", () => {
        const {db} = makeDb("collationMultiViewGraphLookup");

        assert.commandWorked(
            db.nodes.insert([
                {_id: "a", parent: null},
                {_id: "b", parent: "a"},
            ]),
        );
        assert.commandWorked(db.other.insert([{_id: "a"}]));

        // Top-level view: case-insensitive collation.
        makeView(db, "nodesView", "nodes", [], ciCollation);
        // Foreign view: simple (different) collation.
        makeView(db, "otherView", "other", []);

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: "nodesView",
                pipeline: [
                    {
                        $graphLookup: {
                            from: "otherView",
                            startWith: "$parent",
                            connectFromField: "parent",
                            connectToField: "_id",
                            as: "ancestors",
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.OptionNotSupportedOnView,
        );
    });
});
