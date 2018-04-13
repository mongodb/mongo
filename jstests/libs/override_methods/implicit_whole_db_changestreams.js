/**
 * Loading this file overrides DB.prototype._runCommandImpl with a function that converts any
 * attempt to run $changeStream on a single collection into a whole-db $changeStream filtered by
 * that namespace. Single-collection $changeStream rules regarding internal collections and views
 * are respected. Explicit whole-db or whole-cluster $changeStreams, as well as non-$changeStream
 * commands and commands which explicitly request to be exempted from modification by setting the
 * 'noPassthrough' flag, are passed through as-is.
 */

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

// Helper function which tests can call to explicitly request that the command not be modified by
// the passthrough code. When defined, ChangeStreamTest will adopt this as its default runCommand
// implementation to allow individual tests to exempt themselves from modification.
const changeStreamPassthroughAwareRunCommand = (db, cmdObj, noPassthrough) =>
    db.runCommand(cmdObj, undefined, undefined, noPassthrough);

// Defines a set of functions to validate incoming $changeStream requests and transform
// single-collection streams into equivalent whole-db streams. Separating these functions allows the
// runCommand override to generically upconvert $changeStream requests, and the
// ChangeStreamPassthroughHelpers may themselves be overridden by other passthroughs in order to
// alter the behaviour of runCommand.
const ChangeStreamPassthroughHelpers = {
    isValidChangeStreamRequest: function(db, cmdObj) {
        // Determine whether this command is a valid $changeStream aggregation on a single
        // collection or database.
        if (!(cmdObj && cmdObj.aggregate && Array.isArray(cmdObj.pipeline) &&
              cmdObj.pipeline.length > 0 && typeof cmdObj.pipeline[0].$changeStream == "object" &&
              cmdObj.pipeline[0].$changeStream.constructor === Object)) {
            return false;
        }
        // Single-collection and whole-db streams cannot be opened on internal databases.
        if (db.getName() == "admin" || db.getName() == "config" || db.getName() == "local") {
            return false;
        }
        // If the client's $changeStream spec already contains everything we intend to modify, pass
        // the command through as-is.
        const testSpec = this.changeStreamSpec(), csParams = Object.keys(testSpec);
        if (csParams.length > 0 &&
            csParams.every((csParam) =>
                               testSpec[csParam] === cmdObj.pipeline[0].$changeStream[csParam])) {
            return false;
        }
        // The remaining checks are only relevant to single-collection streams. If the 'aggregate'
        // field of the command object is not a string, validate that it is equal to 1.
        if (typeof cmdObj.aggregate !== 'string') {
            return cmdObj.aggregate == 1;
        }
        // Single-collection streams cannot be opened on internal collections in any database.
        if (cmdObj.aggregate.startsWith("system.")) {
            return false;
        }
        // Single-collection streams cannot be opened on views.
        if (FixtureHelpers.getViewDefinition(db, cmdObj.aggregate)) {
            return false;
        }
        // This is a well-formed request.
        return true;
    },
    // All valid single-collection change stream requests are upconvertable in this passthrough.
    isUpconvertableChangeStreamRequest: function(db, cmdObj) {
        return this.isValidChangeStreamRequest(db, cmdObj) &&
            (typeof cmdObj.aggregate === 'string');
    },
    nsMatchFilter: function(db, collName) {
        return {
            $match: {
                $or: [
                    {"ns.db": db.getName(), "ns.coll": collName},
                    {"to.db": db.getName(), "to.coll": collName},
                    {operationType: "invalidate"}
                ]
            }
        };
    },
    execDBName: function(db) {
        return db.getName();
    },
    changeStreamSpec: function() {
        return {};
    },
    upconvertChangeStreamRequest: function(db, cmdObj) {
        // Take a copy of the command object such that the original is not altered.
        cmdObj = Object.assign({}, cmdObj);

        // To convert this command into a whole-db stream, we insert a $match stage just after
        // the $changeStream stage that filters by database and collection name, and we update
        // the command's execution 'namespace' to 1.
        let pipeline = [{
            $changeStream:
                Object.assign({}, cmdObj.pipeline[0].$changeStream, this.changeStreamSpec())
        }];
        pipeline.push(this.nsMatchFilter(db, cmdObj.aggregate));
        pipeline = pipeline.concat(cmdObj.pipeline.slice(1));
        cmdObj.pipeline = pipeline;
        cmdObj.aggregate = 1;

        return [this.execDBName(db), cmdObj];
    },
    upconvertGetMoreRequest: function(db, cmdObj) {
        return [this.execDBName(db), Object.assign({}, cmdObj, {collection: "$cmd.aggregate"})];
    }
};

(function() {
    'use strict';

    const originalRunCommandImpl = DB.prototype._runCommandImpl;
    const originalRunCommand = DB.prototype.runCommand;

    const upconvertedCursors = new Set();

    const db = null;

    const passthroughRunCommandImpl = function(dbName, cmdObj, options) {
        // Check whether this command is an upconvertable $changeStream request.
        const upconvertCursor =
            ChangeStreamPassthroughHelpers.isUpconvertableChangeStreamRequest(this, cmdObj);
        if (upconvertCursor) {
            [dbName, cmdObj] =
                ChangeStreamPassthroughHelpers.upconvertChangeStreamRequest(this, cmdObj);
        }

        // If the command is a getMore, it may be a $changeStream that we upconverted to run
        // whole-db. Ensure that we update the 'collection' field to be the collectionless
        // namespace.
        if (cmdObj && cmdObj.getMore && upconvertedCursors.has(cmdObj.getMore.toString())) {
            [dbName, cmdObj] = ChangeStreamPassthroughHelpers.upconvertGetMoreRequest(this, cmdObj);
        }

        // Pass the modified command to the original runCommand implementation.
        const res = originalRunCommandImpl.apply(this, [dbName, cmdObj, options]);

        // Record the upconverted cursor ID so that we can adjust subsequent getMores.
        if (upconvertCursor && res.cursor && res.cursor.id > 0) {
            upconvertedCursors.add(res.cursor.id.toString());
        }

        return res;
    };

    // Redirect the Collection's 'watch' function to use the whole-DB version. Although calls to the
    // shell helpers will ultimately resolve to the overridden runCommand anyway, we need to
    // override the helpers to ensure that the DB.watch function itself is exercised by the
    // passthrough wherever Collection.watch is called.
    DBCollection.prototype.watch = function(pipeline, options) {
        pipeline = Object.assign([], pipeline);
        pipeline.unshift(
            ChangeStreamPassthroughHelpers.nsMatchFilter(this.getDB(), this.getName()));
        return this.getDB().watch(pipeline, options);
    };

    // Override DB.runCommand to use the custom or original _runCommandImpl.
    DB.prototype.runCommand = function(cmdObj, extra, queryOptions, noPassthrough) {
        this._runCommandImpl = (noPassthrough ? originalRunCommandImpl : passthroughRunCommandImpl);
        return originalRunCommand.apply(this, [cmdObj, extra, queryOptions]);
    };
}());
