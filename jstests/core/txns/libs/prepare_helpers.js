/**
 * Helper functions for testing prepared transactions.
 *
 * @tags: [uses_transactions]
 *
 */
const PrepareHelpers = (function() {

    /**
     * Prepares the active transaction on the session. This expects the 'prepareTransaction' command
     * to succeed and return a non-null 'prepareTimestamp'.
     *
     * @return {Timestamp} the transaction's prepareTimestamp
     */
    function prepareTransaction(session) {
        assert(session);

        const res = assert.commandWorked(
            session.getDatabase('admin').adminCommand({prepareTransaction: 1}));
        assert(res.prepareTimestamp,
               "prepareTransaction did not return a 'prepareTimestamp': " + tojson(res));
        const prepareTimestamp = res.prepareTimestamp;
        assert(prepareTimestamp instanceof Timestamp,
               'prepareTimestamp was not a Timestamp: ' + tojson(res));
        assert.neq(
            prepareTimestamp, Timestamp(0, 0), "prepareTimestamp cannot be null: " + tojson(res));
        return prepareTimestamp;
    }

    /**
     * Commits the active transaction on the session.
     *
     * @return {object} the response to the 'commitTransaction' command.
     */
    function commitTransaction(session, commitTimestamp) {
        assert(session);

        const res = session.getDatabase('admin').adminCommand(
            {commitTransaction: 1, commitTimestamp: commitTimestamp});

        // End the transaction on the shell session.
        if (res.ok) {
            session.commitTransaction();
        } else {
            session.abortTransaction_forTesting();
        }
        return res;
    }

    /**
     * Commits the active transaction on the session at a commitTimestamp that is greater than
     * the transaction's prepareTimestamp.
     *
     * This is a temporary function that should be used to commit prepared transactions
     * until we allow the stable timestamp to move past the oldest active prepare timestamp.
     *
     * @return {object} the response to the 'commitTransaction' command.
     */
    function commitTransactionAfterPrepareTS(session, prepareTimestamp) {
        assert(session);

        // Add 1 to the increment so that the commitTimestamp is "after" the prepareTimestamp.
        const commitTimestamp =
            Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);

        return this.commitTransaction(session, commitTimestamp);
    }

    /**
     * Creates a session object on the given connection with the provided 'lsid'.
     *
     * @return {session} the session created.
     */
    function createSessionWithGivenId(conn, lsid, sessionOptions = {}) {
        const session = conn.startSession(sessionOptions);

        const oldId = session._serverSession.handle.getId();
        print("Overriding sessionID " + tojson(oldId) + " with " + tojson(lsid) + " for test.");
        session._serverSession.handle.getId = () => lsid;

        return session;
    }

    return {
        prepareTransaction: prepareTransaction,
        commitTransaction: commitTransaction,
        commitTransactionAfterPrepareTS: commitTransactionAfterPrepareTS,
        createSessionWithGivenId: createSessionWithGivenId,
    };
})();
