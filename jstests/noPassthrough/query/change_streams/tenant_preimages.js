/**
 * Change stream pre-images are stored in a single tenant-less collection
 * ('config.system.preimages').
 * An update or delete on a tenant-scoped collection with 'changeStreamPreAndPostImages' enabled
 * must be handled normally and write the pre-image to the shared, tenant-less collection.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   # A client cannot address a tenant-scoped namespace directly. With
 *   # 'featureFlagRequireTenantID' enabled, applyOps derives each operation's namespace from the
 *   # command's database tenant rather than from the per-operation 'tid' field, so this setup cannot
 *   # be driven from the shell. The test is therefore skipped when that flag is enabled (e.g. under
 *   # --runAllFeatureFlagTests).
 *   featureFlagRequireTenantID_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// The collection is tenant-scoped and cannot be addressed without a security token, so the
// standard collection validation on shutdown cannot list it.
TestData.skipCollectionAndIndexValidation = true;

describe("Pre-image writes on a tenant-scoped collection write to the shared collection", function () {
    const kDbName = jsTestName();
    const kCollName = "tenant_coll";
    const kTenantId = ObjectId();

    // Tenant-prefixed namespace as it is stored in the catalog and the oplog.
    const kTenantCollNs = kTenantId + "_" + kDbName + "." + kCollName;

    let rst;
    let adminDB;

    function runApplyOps(ops, oplogApplicationMode) {
        const cmd = {applyOps: ops};
        if (oplogApplicationMode) {
            cmd.oplogApplicationMode = oplogApplicationMode;
        }
        return adminDB.runCommand(cmd);
    }

    function insertDoc(id) {
        assert.commandWorked(
            adminDB.runCommand({applyOps: [{op: "i", ns: kTenantCollNs, o: {_id: id}}]}),
        );
    }

    function assertPreImage(preImage) {
        const res = adminDB
            .getSiblingDB("config")
            .getCollection("system.preimages")
            .find({preImage})
            .toArray();
        assert.eq(1, res.length, {res});
    }

    before(function () {
        // The per-operation 'tid' field of applyOps is honored only with multitenancy support.
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {multitenancySupport: true},
            },
        });
        rst.startSet();
        rst.initiate();

        adminDB = rst.getPrimary().getDB("admin");

        // Under disaggregated storage collections use replicated record ids. An oplog entry for
        // such a collection applied outside of applyOps command mode must carry the document's
        // 'rid', but the applyOps command strips the 'rid' field from its operations. Disable
        // replicated record ids so that the secondary-mode operation below can be applied without
        // a 'rid'.
        const overrideRecordIdsReplicatedFalse = configureFailPoint(
            rst.getPrimary(),
            "overrideRecordIdsReplicatedFalse",
        );

        // Arm the tenant-scoped collection with change stream pre-images.
        assert.commandWorked(
            adminDB.runCommand({
                applyOps: [
                    {
                        op: "c",
                        ns: kDbName + ".$cmd",
                        o: {create: kCollName, changeStreamPreAndPostImages: {enabled: true}},
                        tid: kTenantId,
                    },
                ],
            }),
        );

        overrideRecordIdsReplicatedFalse.off();
    });

    after(function () {
        if (rst) {
            rst.stopSet();
        }
    });

    it("writes a pre-image for an update", function () {
        insertDoc(0);
        assert.commandWorked(
            runApplyOps([{op: "u", ns: kTenantCollNs, o: {$set: {x: 1}}, o2: {_id: 0}}]),
        );
        assertPreImage({_id: 0});
    });

    it("writes a pre-image for a delete", function () {
        insertDoc(1);
        assert.commandWorked(runApplyOps([{op: "d", ns: kTenantCollNs, o: {_id: 1}}]));
        assertPreImage({_id: 1});
    });

    it("writes pre-images for a multi-entry applyOps", function () {
        insertDoc(2);
        insertDoc(3);
        assert.commandWorked(
            runApplyOps([
                {op: "u", ns: kTenantCollNs, o: {$set: {x: 2}}, o2: {_id: 2}},
                {op: "d", ns: kTenantCollNs, o: {_id: 3}},
            ]),
        );
        assertPreImage({_id: 2});
        assertPreImage({_id: 3});
    });

    // Applying the operation in the secondary oplog application mode exercises the secondary /
    // recovery pre-image path (writeChangeStreamPreImage in oplog.cpp). The operation carries a
    // nonempty 'tid'.
    it("writes a pre-image when applied in secondary oplog application mode", function () {
        insertDoc(4);
        assert.commandWorked(
            runApplyOps(
                [{op: "u", ns: kTenantCollNs, o: {$set: {x: 4}}, o2: {_id: 4}, tid: kTenantId}],
                "Secondary",
            ),
        );

        // The applyOps command records the pre-image both through the secondary/recovery path and
        // through the regular op observer path (because it also writes its own oplog entry), so
        // more than one pre-image may be present for the same document.
        const res = adminDB
            .getSiblingDB("config")
            .getCollection("system.preimages")
            .find({preImage: {_id: 4}})
            .toArray();
        assert.gte(res.length, 1, {res});
    });
});
