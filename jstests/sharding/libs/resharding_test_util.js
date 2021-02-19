/*
 * Utilities for reshardCollection testing.
 */
var ReshardingTestUtil = (function() {
    /**
     * Returns whether the shard is 'done' aborting the resharding operation with
     * 'abortReason.code'.
     */
    const shardDoneAbortingWithCode = function(shardEntry, errorCode) {
        return shardEntry["abortReason"] && shardEntry["abortReason"]["code"] &&
            shardEntry["abortReason"]["code"] === errorCode && shardEntry["state"] === "done";
    };

    /**
     * Confirms all resharding participants eventually write their abortReason with state 'done' to
     * the main CoordinatorDocument on the configsvr.
     *
     * Each participant shard should eventually report its abortReason to
     * its corresponding entry on the CoordinatorDocument -
     * config.reshardingOperations.recipientShards[shardName] for recipients and
     * config.reshardingOperations.donorShards[shardName] for donors.
     */
    const assertAllParticipantsReportAbortToCoordinator = function(configsvr, nss, errCode) {
        const reshardingOperationsCollection =
            configsvr.getCollection("config.reshardingOperations");
        assert.soon(
            () => {
                const coordinatorDoc = reshardingOperationsCollection.findOne({nss});
                assert(coordinatorDoc);
                // Iterate over both the recipientShards and donorShards and check that every shard
                // entry is in state 'done' and contains an abortReason with the errCode.
                for (const shardEntry
                         of [...coordinatorDoc.recipientShards, ...coordinatorDoc.donorShards]) {
                    if (!shardDoneAbortingWithCode(shardEntry, errCode)) {
                        return false;
                    }
                }
                return true;
            },
            () => {
                return "Not all participants reported to the configsvr's CoordinatorDocument" +
                    " they are done aborting with code " + errCode + ". Got CoordinatorDocument: " +
                    tojson(reshardingOperationsCollection.find().toArray());
            });
    };

    /**
     * Asserts that the particpant eventually reports their abortReason with state 'done' locally
     * to its ReshardingDonorDocument or ReshardingRecipientDocument, pending the participantType.
     *
     * Not exposed to users of ReshardingTestUtil, users should call assertRecipientAbortsLocally or
     * assertDonorAbortsLocally instead.
     */
    const assertParticipantAbortsLocally = function(
        shardConn, shardName, nss, abortReason, participantType) {
        const localOpsCollection =
            shardConn.getCollection(`config.localReshardingOperations.${participantType}`);

        assert.soon(
            () => {
                return localOpsCollection.findOne(
                           {nss, state: "done", "abortReason.code": abortReason}) !== null;
            },
            () => {
                return participantType + " shard " + shardName +
                    " never transitioned to an done state with abortReason " + abortReason + ": " +
                    tojson(localDonorOpsCollection.findOne());
            });
    };

    const assertRecipientAbortsLocally = function(shardConn, shardName, nss, abortReason) {
        return assertParticipantAbortsLocally(shardConn, shardName, nss, abortReason, "recipient");
    };

    const assertDonorAbortsLocally = function(shardConn, shardName, nss, abortReason) {
        return assertParticipantAbortsLocally(shardConn, shardName, nss, abortReason, "donor");
    };

    return {
        assertAllParticipantsReportAbortToCoordinator,
        assertRecipientAbortsLocally,
        assertDonorAbortsLocally
    };
})();
