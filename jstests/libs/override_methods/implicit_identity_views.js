/**
 * Loading this file overrides DB.prototype.getCollection() with a function that creates an identity
 * view on it before returning it, and Mongo.prototype.runCommand() with a function that redirects
 * the command to the view if it is a read command (i.e. find, aggregate, count, distinct).
 */

import {getCommandName, isSystemCollectionName} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Save a reference to the original runCommand and getCollection methods in the IIFE's scope.
// This scoping allows the original methods to be called by the override below.
const originalGetCollection = DB.prototype.getCollection;
const originalRunCommand = Mongo.prototype.runCommand;

const viewNameSuffix = "_identity_view";

DB.prototype.getCollection = function () {
    const collection = originalGetCollection.apply(this, arguments);

    if (isSystemCollectionName(collection.getName())) {
        return collection;
    }

    // Create an identity view for the collection.
    this.runCommand({
        create: collection.getName() + viewNameSuffix,
        viewOn: collection.getName(),
        pipeline: [],
    });
    return collection;
};

Mongo.prototype.runCommand = function (dbName, cmdObj, options) {
    if (typeof cmdObj !== "object" || cmdObj === null) {
        return originalRunCommand.apply(this, arguments);
    }

    const command = getCommandName(cmdObj);
    if (!["count", "distinct", "find", "aggregate"].includes(command)) {
        // Not a read command.
        return originalRunCommand.apply(this, arguments);
    }

    if (cmdObj.hasOwnProperty("collation")) {
        // A view's default collation cannot be overridden, so this will very likely fail.
        return originalRunCommand.apply(this, arguments);
    }

    if (cmdObj.hasOwnProperty("$_requestResumeToken")) {
        // $_requestResumeToken is ignored when run on a view, which will lead to incorrect test
        // behavior.
        return originalRunCommand.apply(this, arguments);
    }

    const collectionName = cmdObj[command];
    if (
        !collectionName ||
        !(typeof collectionName === "string") ||
        isSystemCollectionName(collectionName) ||
        collectionName.startsWith("oplog")
    ) {
        return originalRunCommand.apply(this, arguments);
    }

    // Special handling for find to avoid attempting to run incompatible options on a view.
    if (cmdObj.hasOwnProperty("find")) {
        const optionsDisallowedOnViews = [
            "max",
            "min",
            "noCursorTimeout",
            "returnKey",
            "showRecordId",
            "singleBatch",
            "tailable",
        ];
        for (const disallowedOption of optionsDisallowedOnViews) {
            if (cmdObj.hasOwnProperty(disallowedOption)) {
                return originalRunCommand.apply(this, arguments);
            }
        }
    }
    // Special handling for aggregate to avoid attempting to run incompatible stages on a view.
    else if (cmdObj.hasOwnProperty("aggregate")) {
        const stagesDisallowedOnViews = [
            "$collStats",
            "$currentOp",
            "$documents",
            "$geoNear",
            "$listCatalog",
            "$indexStats",
            "$listLocalSessions",
            "$listSampledQueries",
            "$listSessions",
            "$planCacheStats",
            "$querySettings",
            "$queryStats",
        ];
        for (const stageSpec of cmdObj.pipeline) {
            if (typeof stageSpec !== "object" || stageSpec === null) {
                continue;
            }

            for (const disallowedStage of stagesDisallowedOnViews) {
                if (stageSpec.hasOwnProperty(disallowedStage)) {
                    return originalRunCommand.apply(this, arguments);
                }
            }
        }
    }

    // Update the target collection name to be our identity view.
    cmdObj[command] = cmdObj[command] + viewNameSuffix;
    return originalRunCommand.apply(this, arguments);
};

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/implicit_identity_views.js");
