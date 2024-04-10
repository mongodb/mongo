// Randomly resets the cluster wide write concern to either w:1 or w:majority.
const defaultWriteConcern =
    (Math.random() < 0.5) ? {w: 1, wtimeout: 0} : {w: 'majority', wtimeout: 0};

// Unsetting read/write settings. This command will also cause the server to refresh and get
// the new settings. A standalone, shard server, or old version will return an error; ignore it.
const result = db.adminCommand({
    setDefaultRWConcern: 1,
    defaultReadConcern: {},
    defaultWriteConcern: defaultWriteConcern,
    writeConcern: {w: 1}
});
assert.commandWorkedOrFailedWithCode(result,
                                     TestData.testingReplicaSetEndpoint
                                         ? [ErrorCodes.NotWritablePrimary]
                                         : [51300, 51301, ErrorCodes.IDLUnknownField]);
if (result.ok) {
    jsTestLog("Resetting the global cluster wide write concern to " + tojson(defaultWriteConcern));
}
