// Contains functions for testing the change collections.

// Verifies that the oplog and change collection entries are the same for the provided tenant
// 'tenantId' for the specified timestamp window:- (startOplogTimestamp, endOplogTimestamp].
function verifyChangeCollectionEntries(
    connection, startOplogTimestamp, endOplogTimestamp, tenantId) {
    // Fetch the oplog documents for the provided tenant for the specified timestamp window. Note
    // that the startOplogTimestamp is expected to be just before the first write, while the
    // endOplogTimestamp is expected to be the timestamp of the final write in the test.
    const oplogColl = connection.getDB("local").oplog.rs;
    const oplogEntries = oplogColl
                             .find({
                                 $and: [
                                     {ts: {$gt: startOplogTimestamp}},
                                     {ts: {$lte: endOplogTimestamp}},
                                     {tid: tenantId}
                                 ]
                             })
                             .toArray();

    // Fetch all documents from the tenant's change collection for the specified timestamp window.
    const changeCollectionEntries =
        assert
            .commandWorked(connection.getDB("config").runCommand({
                find: "system.change_collection",
                filter:
                    {$and: [{_id: {$gt: startOplogTimestamp}}, {_id: {$lte: endOplogTimestamp}}]},
                batchSize: 1000000,
                $tenant: tenantId
            }))
            .cursor.firstBatch;

    // Verify that the number of documents returned by the oplog and the tenant's change collection
    // are exactly the same.
    assert.eq(oplogEntries.length,
              changeCollectionEntries.length,
              "Number of entries in the oplog and the change collection with tenantId: " +
                  tenantId + " is not the same. Oplog has total " + oplogEntries.length +
                  " entries , change collection has total " + changeCollectionEntries.length +
                  " entries, change collection entries " + tojson(changeCollectionEntries));

    // Verify that the documents in the change collection are exactly the same as the oplog for a
    // particular tenant.
    for (let idx = 0; idx < oplogEntries.length; idx++) {
        const oplogEntry = oplogEntries[idx];
        const changeCollectionEntry = changeCollectionEntries[idx];

        // Remove the '_id' field from the change collection as oplog does not have it.
        assert(changeCollectionEntry.hasOwnProperty("_id"));
        assert.eq(timestampCmp(changeCollectionEntry._id, oplogEntry.ts),
                  0,
                  "Change collection with tenantId: " + tenantId +
                      " '_id' field: " + tojson(changeCollectionEntry._id) +
                      " is not same as the oplog 'ts' field: " + tojson(oplogEntry.ts));
        delete changeCollectionEntry["_id"];

        // Verify that the oplog and change collecton entry (after removing the '_id') field are
        // the same.
        assert.eq(oplogEntry,
                  changeCollectionEntry,
                  "Oplog and change collection with tenantId: " + tenantId +
                      " entries are not same. Oplog entry: " + tojson(oplogEntry) +
                      ", change collection entry: " + tojson(changeCollectionEntry));
    }
}

// A class that sets up the multitenant environment to enable change collections on the replica set.
// This class also provides helpers that are commonly used when working with change collections.
class ChangeStreamMultitenantReplicaSetTest extends ReplSetTest {
    constructor(config = {}) {
        // Instantiate the 'ReplSetTest' with 'serverless' as an option.
        super(Object.assign({name: "ChangeStreamMultitenantReplicaSetTest", serverless: true},
                            config));

        // A dictionary of parameters required for multitenancy.
        this._multitenancyParameters = {
            featureFlagServerlessChangeStreams: true,
            multitenancySupport: true,
            featureFlagSecurityToken: true,
            featureFlagRequireTenantID: true
        };

        const nodeOptions = config.nodeOptions || {};
        const setParameter =
            Object.assign({}, nodeOptions.setParameter || {}, this._multitenancyParameters);
        this.startSet({setParameter});
        this.initiate();

        // Create a root user within the multitenant environment to enable passing '$tenant' to
        // commands.
        assert.commandWorked(this.getPrimary().getDB("admin").runCommand(
            {createUser: "root", pwd: "pwd", roles: ["root"]}));

        // Unfortunately, ES6 class inheritance doesn't play all that nicely with legacy "Function"
        // classes. As such, overriding an instance method and calling the superclass method does
        // not work properly. We can fake this by holding onto a reference to the "super" add
        // method (ensuring that we bind to the context in this class to avoid issues with the
        // method being invoked in the wrong context) and call it from our override below.
        // If and when ReplSetTest is refactored to use ES6 classes, we can get rid of this madness.
        const superAdd = this.add.bind(this);

        // Adds a node to the replica set with the provided configuration 'config'.
        this.add = (config) => {
            // Get the a copy of the 'config' dictionary and add required multitenancy flags to it.
            const nodeConfig = Object.assign({serverless: true}, config);
            nodeConfig.setParameter =
                Object.assign({}, nodeConfig.setParameter || {}, this._multitenancyParameters);

            // Initiate the replica set with the newly added node.
            return superAdd(nodeConfig);
        };
    }

    // Returns a connection to the 'hostAddr' with 'tenantId' stamped to it for the created user.
    static getTenantConnection(hostAddr,
                               tenantId,
                               user = ObjectId().str,
                               userRoles = [{role: 'readWriteAnyDatabase', db: 'admin'}]) {
        const tokenConn = new Mongo(hostAddr);

        // This method may be called on the secondary connection, as such, enable reading on the
        // secondary. This will be no-op on the primary.
        tokenConn.setSecondaryOk();

        const adminDb = tokenConn.getDB("admin");

        // Login to the root user with 'ActionType::useTenant' such that the '$tenant' can be
        // used.
        assert(adminDb.auth("root", "pwd"));

        // Create the user with the provided roles if it does not exist.
        const existingUser =
            assert
                .commandWorked(adminDb.runCommand(
                    {find: "system.users", filter: {user: user}, $tenant: tenantId}))
                .cursor.firstBatch;
        if (existingUser.length === 0) {
            assert.commandWorked(
                tokenConn.getDB("$external")
                    .runCommand({createUser: user, '$tenant': tenantId, roles: userRoles}));
        }

        // Set the provided tenant id into the security token for the user.
        tokenConn._setSecurityToken(
            _createSecurityToken({user: user, db: '$external', tenant: tenantId}));

        // Logout the root user to avoid multiple authentication.
        tokenConn.getDB("admin").logout();

        return tokenConn;
    }

    // Sets the change stream state for the provided tenant connection.
    setChangeStreamState(tenantConn, enabled) {
        assert.commandWorked(
            tenantConn.getDB("admin").runCommand({setChangeStreamState: 1, enabled: enabled}));
    }

    // Returns the change stream state for the provided tenant connection.
    getChangeStreamState(tenantConn) {
        return assert.commandWorked(tenantConn.getDB("admin").runCommand({getChangeStreamState: 1}))
            .enabled;
    }
}
