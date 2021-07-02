'use strict';

/**
 * Each thread uses its own LSID and performs `findAndModify`s with retries on documents while the
 * `storeFindAndModifyImagesInSideCollection` server parameter gets flipped.
 *
 * @tags: [requires_replication, requires_non_retryable_commands, uses_transactions];
 */
var $config = (function() {
    var data = {
        numDocs: 100,
    };

    var states = (function() {
        function init(db, collName) {
            this._lastTxnId = 0;
            this._lsid = UUID();
        }

        function findAndModifyUpsert(db, collName) {
            // `auto_retry_transactions` is not compatible with explicitly testing retryable writes.
            // This avoids issues regarding the multi_stmt tasks.
            fsm.forceRunningOutsideTransaction(this);

            this._lastTxnId += 1;
            this._lastCmd = {
                findandmodify: collName,
                lsid: {id: this._lsid},
                txnNumber: NumberLong(this._lastTxnId),
                stmtId: NumberInt(1),
                query: {_id: Math.round(Math.random() * this.numDocs)},
                new: Math.random() > 0.5,
                upsert: true,
                update: {$inc: {counter: 1}},
            };
            // The lambda passed into 'assert.soon' does not have access to 'this'.
            let data = {"lastCmd": this._lastCmd};
            assert.soon(function() {
                try {
                    data.lastResponse = assert.commandWorked(db.runCommand(data.lastCmd));
                    return true;
                } catch (e) {
                    if (e.code === ErrorCodes.DuplicateKey) {
                        // When run under multiversion suites that operate on v4.4 binary mongods,
                        // it is possible that two threads race to upsert the same '_id' into the
                        // same collection, a scenario described in SERVER-47212. In this case, we
                        // retry the upsert.
                        checkFCV(db.getSiblingDB('admin'), '4.4');
                        print('Encountered DuplicateKey error. Retrying upsert:' +
                              tojson(data.lastCmd));
                        return false;
                    }
                    throw e;
                }
            });
            this._lastResponse = data.lastResponse;
        }

        function findAndModifyUpdate(db, collName) {
            // `auto_retry_transactions` is not compatible with explicitly testing retryable writes.
            // This avoids issues regarding the multi_stmt tasks.
            fsm.forceRunningOutsideTransaction(this);

            this._lastTxnId += 1;
            this._lastCmd = {
                findandmodify: collName,
                lsid: {id: this._lsid},
                txnNumber: NumberLong(this._lastTxnId),
                stmtId: NumberInt(1),
                query: {_id: Math.round(Math.random() * this.numDocs)},
                new: Math.random() > 0.5,
                upsert: false,
                update: {$inc: {counter: 1}},
            };
            this._lastResponse = assert.commandWorked(db.runCommand(this._lastCmd));
        }

        function findAndModifyDelete(db, collName) {
            // `auto_retry_transactions` is not compatible with explicitly testing retryable writes.
            // This avoids issues regarding the multi_stmt tasks.
            fsm.forceRunningOutsideTransaction(this);

            this._lastTxnId += 1;
            this._lastCmd = {
                findandmodify: collName,
                lsid: {id: this._lsid},
                txnNumber: NumberLong(this._lastTxnId),
                stmtId: NumberInt(1),
                query: {_id: Math.round(Math.random() * this.numDocs)},
                // Deletes may not ask for the postImage
                new: false,
                remove: true,
            };
            this._lastResponse = assert.commandWorked(db.runCommand(this._lastCmd));
        }

        function findAndModifyRetry(db, collName) {
            // `auto_retry_transactions` is not compatible with explicitly testing retryable writes.
            // This avoids issues regarding the multi_stmt tasks.
            fsm.forceRunningOutsideTransaction(this);

            assert(this._lastCmd);
            assert(this._lastResponse);

            let response = assert.commandWorked(db.runCommand(this._lastCmd));
            let debugMsg = {
                "TID": this.tid,
                "LastCmd": this._lastCmd,
                "LastResponse": this._lastResponse,
                "Response": response
            };
            assert.eq(this._lastResponse.hasOwnProperty("lastErrorObject"),
                      response.hasOwnProperty("lastErrorObject"),
                      debugMsg);
            if (response.hasOwnProperty("lastErrorObject") &&
                // If the original command affected `n=1` document, all retries must return
                // identical results. If an original command receives `n=0`, then a retry may find a
                // match and return `n=1`. Only compare `lastErrorObject` and `value` when retries
                // must be identical.
                this._lastResponse["lastErrorObject"].n === 1) {
                assert.eq(
                    this._lastResponse["lastErrorObject"], response["lastErrorObject"], debugMsg);
            }
            assert.eq(this._lastResponse.hasOwnProperty("value"),
                      response.hasOwnProperty("value"),
                      debugMsg);
            if (response.hasOwnProperty("value") && this._lastResponse["lastErrorObject"].n === 1) {
                assert.eq(this._lastResponse["value"], response["value"], debugMsg);
            }

            // Have all workers participate in creating some chaos.
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                storeFindAndModifyImagesInSideCollection: Math.random() > 0.5,
            }));
        }

        return {
            init: init,
            findAndModifyUpsert: findAndModifyUpsert,
            findAndModifyUpdate: findAndModifyUpdate,
            findAndModifyDelete: findAndModifyDelete,
            findAndModifyRetry: findAndModifyRetry
        };
    })();

    var transitions = {
        init: {findAndModifyUpsert: 1.0},
        findAndModifyUpsert: {
            findAndModifyRetry: 3.0,
            findAndModifyUpsert: 1.0,
            findAndModifyUpdate: 1.0,
            findAndModifyDelete: 1.0
        },
        findAndModifyUpdate: {
            findAndModifyRetry: 3.0,
            findAndModifyUpsert: 1.0,
            findAndModifyUpdate: 1.0,
            findAndModifyDelete: 1.0
        },
        findAndModifyDelete: {
            findAndModifyRetry: 3.0,
            findAndModifyUpsert: 1.0,
            findAndModifyUpdate: 1.0,
            findAndModifyDelete: 1.0
        },
        findAndModifyRetry: {
            findAndModifyRetry: 1.0,
            findAndModifyUpsert: 1.0,
            findAndModifyUpdate: 1.0,
            findAndModifyDelete: 1.0
        },
    };

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        states: states,
        transitions: transitions,
        setup: function() {},
    };
})();
