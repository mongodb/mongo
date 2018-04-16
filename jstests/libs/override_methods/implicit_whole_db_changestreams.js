/**
 * Loading this file overrides DB.prototype._runCommandImpl with a function that converts any
 * attempt to run $changeStream on a single collection into a whole-db $changeStream filtered by
 * that namespace. Single-collection $changeStream rules regarding internal collections and views
 * are respected. Explicit whole-db or whole-cluster $changeStreams, as well as non-$changeStream
 * commands and commands which explicitly request to be exempted from modification by setting the
 * 'noPassthrough' flag, are passed through as-is.
 */

// Helper function which tests can call to explicitly request that the command not be modified by
// the passthrough code. When defined, ChangeStreamTest will adopt this as its default runCommand
// implementation to allow individual tests to exempt themselves from modification.
const changeStreamPassthroughAwareRunCommand = (db, cmdObj, noPassthrough) =>
    db.runCommand(cmdObj, undefined, undefined, noPassthrough);

(function() {
    'use strict';

    load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

    const originalRunCommandImpl = DB.prototype._runCommandImpl;
    const originalRunCommand = DB.prototype.runCommand;

    const upconvertedCursors = new Set();

    function isValidChangeStreamRequest(db, cmdObj) {
        // Determine whether this command is a $changeStream aggregation on a single collection.
        if (cmdObj && typeof cmdObj.aggregate === 'string' && Array.isArray(cmdObj.pipeline) &&
            cmdObj.pipeline.length > 0 && cmdObj.pipeline[0].$changeStream) {
            // Single-collection streams cannot be opened on internal databases.
            if (db.getName() == "admin" || db.getName() == "config" || db.getName() == "local") {
                return false;
            }
            // Single-collection streams cannot be opened on internal collections in any database.
            if (cmdObj.aggregate.startsWith("system.")) {
                return false;
            }
            // Single-collection streams cannot be opened on views.
            if (FixtureHelpers.getViewDefinition(db, cmdObj.aggregate)) {
                return false;
            }
            // This is a well-formed single-collection request.
            return true;
        }

        return false;
    }

    const passthroughRunCommandImpl = function(dbName, cmdObj, options) {
        // Check whether this command is a valid $changeStream request.
        let upconvertCursor = isValidChangeStreamRequest(this, cmdObj);
        if (upconvertCursor) {
            // Having validated the legality of the stream, take a copy of the command object such
            // that the original object is not altered.
            cmdObj = Object.assign({}, cmdObj);

            // To convert this command into a whole-db stream, we insert a $match stage just after
            // the $changeStream stage that filters by database and collection name, and we update
            // the command's execution 'namespace' to 1.
            let pipeline = [{$changeStream: Object.assign({}, cmdObj.pipeline[0].$changeStream)}];
            pipeline.push({
                $match: {
                    $or: [
                        {"ns.db": dbName, "ns.coll": cmdObj.aggregate},
                        {operationType: "invalidate"}
                    ]
                }
            });
            pipeline = pipeline.concat(cmdObj.pipeline.slice(1));
            cmdObj.pipeline = pipeline;
            cmdObj.aggregate = 1;
        }

        // If the command is a getMore, it may be a $changeStream that we upconverted to run
        // whole-db. Ensure that we update the 'collection' field to be the collectionless
        // namespace.
        if (cmdObj && cmdObj.getMore && upconvertedCursors.has(cmdObj.getMore.toString())) {
            cmdObj = Object.assign({}, cmdObj, {collection: "$cmd.aggregate"});
        }

        // Pass the modified command to the original runCommand implementation.
        const res = originalRunCommandImpl.apply(this, [dbName, cmdObj, options]);

        // Record the upconverted cursor ID so that we can adjust subsequent getMores.
        if (upconvertCursor && res.cursor && res.cursor.id > 0) {
            upconvertedCursors.add(res.cursor.id.toString());
        }

        return res;
    };

    DB.prototype.runCommand = function(cmdObj, extra, queryOptions, noPassthrough) {
        this._runCommandImpl = (noPassthrough ? originalRunCommandImpl : passthroughRunCommandImpl);
        return originalRunCommand.apply(this, [cmdObj, extra, queryOptions]);
    };
}());
