import {
    withAbortAndRetryOnTransientTxnError
} from "jstests/libs/auto_retry_transaction_in_sharding.js";

export function getReplicaSetURL(db) {
    const rsConfig = assert.commandWorked(db.adminCommand({replSetGetConfig: 1})).config;
    const rsName = rsConfig._id;
    const rsHosts = rsConfig.members.map(member => member.host);
    return `${rsName}/${rsHosts.join(",")}`;
}

export function extractReplicaSetNameAndHosts(primary, rsURL) {
    if (!rsURL) {
        rsURL = getReplicaSetURL(primary);
    }
    const splitURL = rsURL.split("/");
    const rsName = splitURL[0];
    const rsHosts = splitURL[1].split(",");
    const primaryHost = primary.host;
    const host = rsHosts[0].split(":")[0];
    return {rsURL, rsName, rsHosts, primaryHost, host};
}

export function makeReplicaSetConnectionString(
    rsName, rsHosts, defaultDbName, {authDbName, user} = {}) {
    if (authDbName && !user.securityToken) {
        // For tenant users, auth is performed through attaching the security token to each
        // command.
        assert(user.userName);
        assert(user.password);
        return `mongodb://${
            user.userName + ":" + user.password +
            "@"}${rsHosts.join(",")}/${defaultDbName}?authSource=${authDbName}&replicaSet=${
            rsName}&readPreference=secondary`;
    }
    return `mongodb://${rsHosts.join(",")}/${defaultDbName}?replicaSet=${rsName}`;
}

export function makeStandaloneConnectionString(nodeHost, defaultDbName, {authDbName, user} = {}) {
    if (authDbName && !user.securityToken) {
        // For tenant users, auth is performed through attaching the security token to each
        // command.
        assert(user.userName);
        assert(user.password);
        return `mongodb://${
            user.userName + ":" + user.password +
            "@"}${nodeHost}/${defaultDbName}?authSource=${authDbName}`;
    }
    return `mongodb://${nodeHost}/${defaultDbName}`;
}

export function waitForAutoBootstrap(node, keyFile) {
    assert.soon(() => node.adminCommand({hello: 1}).isWritablePrimary);

    const getConfigShardDoc = function() {
        return node.getDB("config").shards.findOne({_id: "config"});
    };
    assert.soonNoExcept(() => {
        const configShardDoc =
            keyFile ? authutil.asCluster(node, keyFile, getConfigShardDoc) : getConfigShardDoc();
        return configShardDoc != null;
    });

    const getShardIdentityDoc = function() {
        return node.getDB("admin").system.version.findOne({_id: "shardIdentity"});
    };
    const shardIdentityDoc =
        keyFile ? authutil.asCluster(node, keyFile, getShardIdentityDoc) : getShardIdentityDoc();
    assert.eq(shardIdentityDoc.shardName, "config", shardIdentityDoc);
}

export const execCtxTypes = {
    kNoSession: 1,
    kNonRetryableWrite: 2,
    kRetryableWrite: 3,
    kTransaction: 4
};

export function runCommands(conn, execCtxType, dbName, collName, cmdFunc) {
    switch (execCtxType) {
        case execCtxTypes.kNoSession: {
            const coll = conn.getDB(dbName).getCollection(collName);
            cmdFunc(coll);
            return;
        }
        case execCtxTypes.kNonRetryableWrite: {
            const session = conn.startSession({retryWrites: false});
            const coll = session.getDatabase(dbName).getCollection(collName);
            cmdFunc(coll);
            session.endSession();
            return;
        }
        case execCtxTypes.kRetryableWrite: {
            const session = conn.startSession({retryWrites: true});
            const coll = session.getDatabase(dbName).getCollection(collName);
            cmdFunc(coll);
            session.endSession();
            return;
        }
        case execCtxTypes.kTransaction: {
            const session = conn.startSession({retryWrites: false});
            const coll = session.getDatabase(dbName).getCollection(collName);
            withAbortAndRetryOnTransientTxnError(session, () => {
                session.startTransaction();
                cmdFunc(coll);
                session.commitTransaction();
            });
            session.endSession();
            return;
        }
        default:
            throw Error("Unknown execution context");
    }
}

export function getCollectionUuid(db, dbName, collName) {
    const listCollectionRes = assert.commandWorked(
        db.getSiblingDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}));
    return listCollectionRes.cursor.firstBatch[0].info.uuid;
}

export function assertShardingMetadataForUnshardedCollectionExists(db, collUuid, dbName, collName) {
    const nss = dbName + "." + collName;
    const configDB = db.getSiblingDB("config");

    const collDoc = configDB.getCollection("collections").findOne({uuid: collUuid});
    assert.neq(collDoc, null);
    assert.eq(collDoc._id, nss, collDoc);
    assert(collDoc.unsplittable, collDoc);
    assert.eq(collDoc.key, {_id: 1}, collDoc);

    const chunkDocs = configDB.getCollection("chunks").find({uuid: collUuid}).toArray();
    assert.eq(chunkDocs.length, 1, chunkDocs);
}

export function assertShardingMetadataForUnshardedCollectionDoesNotExist(db, collUuid) {
    const configDB = db.getSiblingDB("config");

    const collDoc = configDB.getCollection("collections").findOne({uuid: collUuid});
    assert.eq(collDoc, null);

    const chunkDocs = configDB.getCollection("chunks").find({uuid: collUuid}).toArray();
    assert.eq(chunkDocs.length, 0, chunkDocs);
}

export function makeCreateUserCmdObj(user) {
    const cmdObj = {
        createUser: user.userName,
        pwd: user.password,
        roles: user.roles,
    };
    return cmdObj;
}

export function makeCreateRoleCmdObj(role) {
    const cmdObj = {createRole: role.name, roles: role.roles, privileges: role.privileges};
    return cmdObj;
}
