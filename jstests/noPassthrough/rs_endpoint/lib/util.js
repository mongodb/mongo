export function getReplicaSetURL(db) {
    const rsConfig = assert.commandWorked(db.adminCommand({replSetGetConfig: 1})).config;
    const rsName = rsConfig._id;
    const rsHosts = rsConfig.members.map(member => member.host);
    return `${rsName}/${rsHosts.join(",")}`;
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
