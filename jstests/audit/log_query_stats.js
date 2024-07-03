// Verify usage of $queryStats agg stage can be sent to audit log.
// @tags: [requires_fcv_60]

(function() {
'use strict';

load('src/mongo/db/modules/enterprise/jstests/audit/lib/audit.js');

const runTest = function(audit, db, admin) {
    assert.commandWorked(
        admin.runCommand({createUser: "user1", pwd: "pwd", roles: [{role: "root", db: "admin"}]}));
    audit.fastForward();

    // Authentication does not match the audit filter.
    assert(admin.auth({user: "user1", pwd: "pwd"}));
    audit.assertNoNewEntries();

    audit.fastForward();

    // "$queryStats" command with no transform identifiers matches the audit filter.
    assert.commandWorked(
        db.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));
    audit.assertCmd("authCheck", {
        command: "aggregate",
        ns: "admin.$cmd.aggregate",
        args: {
            aggregate: 1,
            pipeline: [{$queryStats: {}}],
            cursor: {},
            lsid: db.getSession().getSessionId(),
            $db: "admin",
        }
    });

    audit.fastForward();

    // "$queryStats" with both transform identifiers matches the audit filter.
    const hmacKey = "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=";
    assert.commandWorked(db.adminCommand({
        aggregate: 1,
        pipeline: [{
            $queryStats:
                {transformIdentifiers: {algorithm: "hmac-sha-256", hmacKey: BinData(8, hmacKey)}}
        }],
        cursor: {}
    }));

    // Query stats adds extra timestamping fields that we don't want to bother with.
    audit.assertEntryRelaxed("authCheck", {
        command: "aggregate",
        ns: "admin.$cmd.aggregate",
        args: {
            aggregate: 1,
            pipeline: [{
                $queryStats: {
                    transformIdentifiers: {
                        algorithm: "hmac-sha-256",
                        // HMAC key is Sensitive, so it should always be redacted.
                        hmacKey: "###"
                    }
                }
            }],
            cursor: {},
            lsid: db.getSession().getSessionId(),
            $db: "admin"
        }
    });

    // Collection drop does not match the audit filter, so no entry is produced.
    db.coll.drop();
    audit.assertNoNewEntries();
    admin.logout();
};

const auditFilterStr = "{'param.args.pipeline.0.$queryStats':{$exists:true}}";
const parameters = {
    auditAuthorizationSuccess: true,
    internalQueryStatsRateLimit: -1
};

{
    const m = MongoRunner.runMongodAuditLogger(
        {auth: "", auditFilter: auditFilterStr, setParameter: parameters});
    const audit = m.auditSpooler();
    const db = m.getDB("test");
    const admin = m.getDB("admin");

    audit.assertCmd = audit.assertEntry;
    runTest(audit, db, admin);
    MongoRunner.stopMongod(m);
}

{
    const st = MongoRunner.runShardedClusterAuditLogger({}, {
        auth: null,
        auditFilter: auditFilterStr,
        setParameter: parameters,
    });
    const auditMongos = st.s0.auditSpooler();
    const db = st.s0.getDB("test");
    const admin = st.s0.getDB("admin");

    // On clusters, clusterTime shows up in the param field. We don't want to worry about that
    // so we run assertEntryRelaxed instead.
    auditMongos.assertCmd = auditMongos.assertEntryRelaxed;
    runTest(auditMongos, db, admin);
    st.stop();
}
})();
