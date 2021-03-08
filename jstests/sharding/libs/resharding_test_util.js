/*
 * Utilities for reshardCollection testing.
 */
var ReshardingTestUtil = (function() {
    /**
     * Returns whether the shard is 'done' aborting the resharding operation with
     * 'abortReason.code'.
     */
    const shardDoneAbortingWithCode = function(shardEntry, errorCode) {
        return shardEntry.mutableState.abortReason &&
            shardEntry.mutableState.abortReason.code === errorCode &&
            shardEntry.mutableState.state === "done";
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
    const assertAllParticipantsReportAbortToCoordinator = function(configsvr, ns, errCode) {
        const reshardingOperationsCollection =
            configsvr.getCollection("config.reshardingOperations");
        assert.soon(
            () => {
                const coordinatorDoc = reshardingOperationsCollection.findOne({ns});
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
        shardConn, shardName, ns, abortReason, participantType) {
        const localOpsCollection =
            shardConn.getCollection(`config.localReshardingOperations.${participantType}`);

        assert.soon(
            () => {
                return localOpsCollection.findOne({
                    ns,
                    "mutableState.state": "done",
                    "mutableState.abortReason.code": abortReason,
                }) !== null;
            },
            () => {
                return participantType + " shard " + shardName +
                    " never transitioned to an done state with abortReason " + abortReason + ": " +
                    tojson(localOpsCollection.findOne());
            });
    };

    const assertRecipientAbortsLocally = function(shardConn, shardName, ns, abortReason) {
        return assertParticipantAbortsLocally(shardConn, shardName, ns, abortReason, "recipient");
    };

    const assertDonorAbortsLocally = function(shardConn, shardName, ns, abortReason) {
        return assertParticipantAbortsLocally(shardConn, shardName, ns, abortReason, "donor");
    };

    return {
        assertAllParticipantsReportAbortToCoordinator,
        assertRecipientAbortsLocally,
        assertDonorAbortsLocally
    };
})();
