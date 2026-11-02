/**
 * Loading this file overrides runCommand with a function that converts any attempt to run
 * $changeStream on a single collection into a whole-db $changeStream filtered by that namespace.
 * Single-collection $changeStream rules regarding internal collections and views are respected.
 * Explicit whole-db or whole-cluster $changeStreams, as well as non-$changeStream commands and
 * commands which explicitly request to be exempted from modification by passing a truthy
 * 'noPassthrough' flag to 'changeStreamPassthroughAwareRunCommand', are passed through as-is.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    ChangeStreamWatchMode,
    getChangeStreamStage,
} from "jstests/libs/query/change_stream_util.js";

// Set while a command that must not be upconverted is in flight. Exempting a command from this
// passthrough must not exempt it from the other runCommand overrides (in particular the transaction
// passthrough): otherwise a change stream opened here would not have its cursor registered as a
// non-transaction cursor, and its subsequent getMores would be wrapped in a transaction.
let skipUpconversion = false;

// Helper function which tests can call to explicitly request that the command not be modified by
// the passthrough code. When defined, ChangeStreamTest will adopt this as its default runCommand
// implementation to allow individual tests to exempt themselves from modification.
globalThis.changeStreamPassthroughAwareRunCommand = (db, cmdObj, noPassthrough) => {
    if (!noPassthrough) {
        return db.runCommand(cmdObj);
    }
    skipUpconversion = true;
    try {
        return db.runCommand(cmdObj);
    } finally {
        skipUpconversion = false;
    }
};

// Defines a set of functions to validate incoming $changeStream requests and transform
// single-collection streams into equivalent whole-db streams. Separating these functions allows the
// runCommand override to generically upconvert $changeStream requests, and the
// ChangeStreamPassthroughHelpers may themselves be overridden by other passthroughs in order to
// alter the behaviour of runCommand.
globalThis.ChangeStreamPassthroughHelpers = {
    isValidChangeStreamRequest: function (db, cmdObj) {
        // Determine whether this command is a valid $changeStream aggregation on a single
        // collection or database.
        const changeStreamStage = getChangeStreamStage(cmdObj);
        if (
            !(
                typeof changeStreamStage === "object" &&
                changeStreamStage !== null &&
                changeStreamStage.constructor === Object
            )
        ) {
            return false;
        }
        // Single-collection and whole-db streams cannot be opened on internal databases.
        if (db.getName() == "admin" || db.getName() == "config" || db.getName() == "local") {
            return false;
        }
        // If the client's $changeStream spec already contains everything we intend to modify, pass
        // the command through as-is.
        const testSpec = this.changeStreamSpec(),
            csParams = Object.keys(testSpec);
        if (
            csParams.length > 0 &&
            csParams.every(
                (csParam) => testSpec[csParam] === cmdObj.pipeline[0].$changeStream[csParam],
            )
        ) {
            return false;
        }
        // The remaining checks are only relevant to single-collection streams. If the 'aggregate'
        // field of the command object is not a string, validate that it is equal to 1.
        if (typeof cmdObj.aggregate !== "string") {
            return cmdObj.aggregate == 1;
        }
        // Single-collection streams cannot be opened on internal collections in any database.
        if (cmdObj.aggregate.startsWith("system.")) {
            return false;
        }
        // Single-collection streams cannot be opened on views. getViewDefinition() issues its own
        // listCollections command, which can itself fail (e.g. a sharded cluster with all shards
        // unreachable) independently of the change stream request we're actually validating. If we
        // can't answer the question, don't block the request on it - fall through and let the
        // upconverted (or original) request surface whatever error is actually relevant.
        try {
            if (FixtureHelpers.getViewDefinition(db, cmdObj.aggregate)) {
                return false;
            }
        } catch (e) {
            jsTest.log.info(
                "implicit_whole_db_changestreams.js: getViewDefinition() failed, proceeding as if not a view",
                {error: e.message},
            );
        }
        // This is a well-formed request.
        return true;
    },
    // All valid single-collection change stream requests are upconvertable in this passthrough.
    isUpconvertableChangeStreamRequest: function (db, cmdObj) {
        return this.isValidChangeStreamRequest(db, cmdObj) && typeof cmdObj.aggregate === "string";
    },
    nsMatchFilter: function (db, collName) {
        return {
            $match: {
                $or: [
                    {"ns.db": db.getName(), "ns.coll": collName},
                    {"to.db": db.getName(), "to.coll": collName},
                    {operationType: "endOfTransaction"},
                    {operationType: "invalidate"},
                ],
            },
        };
    },
    execDBName: function (db) {
        return db.getName();
    },
    changeStreamSpec: function () {
        return {};
    },
    upconvertChangeStreamRequest: function (db, cmdObj) {
        // Take a copy of the command object such that the original is not altered.
        cmdObj = Object.assign({}, cmdObj);

        // To convert this command into a whole-db stream, we insert a $match stage just after
        // the $changeStream stage that filters by database and collection name, and we update
        // the command's execution 'namespace' to 1.
        let pipeline = [
            {
                $changeStream: Object.assign(
                    {},
                    cmdObj.pipeline[0].$changeStream,
                    this.changeStreamSpec(),
                ),
            },
        ];
        pipeline.push(this.nsMatchFilter(db, cmdObj.aggregate));
        pipeline = pipeline.concat(cmdObj.pipeline.slice(1));
        cmdObj.pipeline = pipeline;
        cmdObj.aggregate = 1;

        return [this.execDBName(db), cmdObj];
    },
    upconvertGetMoreRequest: function (db, cmdObj) {
        return [this.execDBName(db), Object.assign({}, cmdObj, {collection: "$cmd.aggregate"})];
    },
    passthroughType: function () {
        return ChangeStreamWatchMode.kDb;
    },
};

const upconvertedCursors = new Set();

function runCommandOverride(conn, dbName, _commandName, cmdObj, func, makeFuncArgs) {
    if (typeof cmdObj !== "object" || cmdObj === null) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // Commands explicitly exempted via 'changeStreamPassthroughAwareRunCommand' are passed through
    // untouched, but still flow through the remaining overrides.
    if (skipUpconversion) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // Only aggregates can carry a $changeStream stage, and only getMores of an already-upconverted
    // cursor need their collection rewritten. Everything else is passed through untouched.
    let csCmdObj = cmdObj.explain ? cmdObj.explain : cmdObj;
    if (csCmdObj.aggregate === undefined && csCmdObj.getMore === undefined) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // The upconversion is namespace-aware, so it needs the database the command was issued on.
    const db = conn.getDB(dbName);

    const upconvertCursor =
        globalThis.ChangeStreamPassthroughHelpers.isUpconvertableChangeStreamRequest(db, csCmdObj);
    if (upconvertCursor) {
        [dbName, csCmdObj] = globalThis.ChangeStreamPassthroughHelpers.upconvertChangeStreamRequest(
            db,
            csCmdObj,
        );
    }

    // If this is an explain, put the upconverted agg back into the explain command.
    if (cmdObj.explain) {
        cmdObj = Object.assign({}, cmdObj, {explain: csCmdObj});
    } else {
        cmdObj = csCmdObj;
    }

    // If the command is a getMore, it may be a $changeStream that we upconverted to run
    // whole-db. Ensure that we update the 'collection' field to be the collectionless
    // namespace.
    if (cmdObj && cmdObj.getMore && upconvertedCursors.has(cmdObj.getMore.toString())) {
        [dbName, cmdObj] = globalThis.ChangeStreamPassthroughHelpers.upconvertGetMoreRequest(
            db,
            cmdObj,
        );
    }

    // Pass the modified command to the original runCommand implementation.
    const res = func.apply(conn, makeFuncArgs(cmdObj, dbName));

    // Record the upconverted cursor ID so that we can adjust subsequent getMores.
    if (upconvertCursor && res.cursor && res.cursor.id > 0) {
        upconvertedCursors.add(res.cursor.id.toString());
    }

    return res;
}

// Redirect the Collection's 'watch' function to use the whole-DB version. Although calls to the
// shell helpers will ultimately resolve to the overridden runCommand anyway, we need to
// override the helpers to ensure that the DB.watch function itself is exercised by the
// passthrough wherever Collection.watch is called.
DBCollection.prototype.watch = function (pipeline, options) {
    pipeline = Object.assign([], pipeline);
    pipeline.unshift(
        globalThis.ChangeStreamPassthroughHelpers.nsMatchFilter(this.getDB(), this.getName()),
    );
    return this.getDB().watch(pipeline, options);
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_whole_db_changestreams.js",
);

OverrideHelpers.overrideRunCommand(runCommandOverride);
