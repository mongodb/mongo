/**
 * A user holding only readWrite on one database must not be able to read another database's
 * documents through a view on a sharded cluster.
 *
 * @tags: [requires_sharding, assumes_auth]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kRejectionCodes = [5491300, ErrorCodes.OptionNotSupportedOnView];

describe("internal-only stage in a view definition on a sharded cluster", function () {
    let st;
    let uATenantA;

    before(function () {
        st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});

        // Use the localhost exception to bootstrap a root user, then create the test user.
        const admin = st.s.getDB("admin");
        assert.commandWorked(
            admin.runCommand({createUser: "root", pwd: "rootpw", roles: ["root"]}),
        );
        assert(admin.auth("root", "rootpw"));

        assert.commandWorked(
            admin.runCommand({
                createUser: "uA",
                pwd: "pwdA",
                roles: [{role: "readWrite", db: "tenantA"}],
            }),
        );

        // Victim data in another database on the same shard, which uA holds no privileges on.
        assert.commandWorked(
            admin
                .getSiblingDB("tenantB")
                .payroll.insert({_id: 1, employee: "victim", ssn: "601-55-1000"}),
        );

        // The attacker holds readWrite on tenantA only.
        const uA = new Mongo(st.s.host);
        assert(uA.getDB("admin").auth("uA", "pwdA"));
        uATenantA = uA.getDB("tenantA");
        assert.commandWorked(uATenantA.pub.insert({_id: 1, v: "A-PUB"}));
    });

    after(function () {
        st.stop();
    });

    it("rejects storing '$setVariableFromSubPipeline' in a view definition", function () {
        const createRes = uATenantA.runCommand({
            create: "exfil",
            viewOn: "pub",
            pipeline: [
                {
                    $setVariableFromSubPipeline: {
                        setVariable: "$$SEARCH_META",
                        pipeline: [
                            {$collStats: {count: {}}},
                            {
                                $lookup: {
                                    from: {db: "local", coll: "oplog.rs"},
                                    pipeline: [
                                        {$match: {ns: "tenantB.payroll"}},
                                        {$project: {_id: 0, ns: 1, o: 1}},
                                    ],
                                    as: "X",
                                },
                            },
                        ],
                    },
                },
                {$replaceWith: {LEAK: "$$SEARCH_META.X"}},
            ],
        });

        assert.commandFailedWithCode(
            createRes,
            kRejectionCodes,
            "a view definition containing an internal-only stage was accepted",
        );
        assert(
            !uATenantA.getCollectionNames().includes("exfil"),
            "a view definition containing an internal-only stage was stored",
        );
    });
});
