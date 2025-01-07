import {sysCollNamePrefix} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

/**
 * Resolves the command name for the given 'cmdObj'.
 */
export function getCommandName(cmdObj) {
    return Object.keys(cmdObj)[0];
}

/**
 * Returns the inner command if 'cmdObj' represents an explain command, or simply 'cmdObj'
 * otherwise.
 */
export function getInnerCommand(cmdObj) {
    const isExplain = "explain" in cmdObj;
    if (!isExplain) {
        return cmdObj;
    }

    if (typeof cmdObj.explain === "object") {
        return cmdObj.explain;
    }

    const {explain, ...cmdWithoutExplain} = cmdObj;
    return cmdWithoutExplain;
}

/**
 *  Returns the explain command object for the given 'cmdObj'.
 */
export function getExplainCommand(cmdObj) {
    const isAggregateCmd = getCommandName(cmdObj) === "aggregate";
    return isAggregateCmd ? {explain: {...cmdObj, cursor: {}}} : {explain: cmdObj};
}

/**
 * Resolves the collection name for the given 'cmdObj'. If the command targets a view, then this
 * will recursively find the underlying collection's name and return it. Returns 'undefined' if the
 * collection does not exist.
 */
export function getCollectionName(db, cmdObj) {
    try {
        const collectionsInfo = db.getCollectionInfos();
        if (!collectionsInfo || collectionsInfo.length === 0) {
            return undefined;
        }
        let viewOn;
        let name = cmdObj[getCommandName(cmdObj)];
        do {
            let collInfo = collectionsInfo.find(c => c.name === name);
            if (!collInfo) {
                name = undefined;
            }
            viewOn = collInfo?.options?.viewOn;
            if (viewOn) {
                name = viewOn;
            }
        } while (viewOn);
        return name;
    } catch (ex) {
        switch (ex.code) {
            case ErrorCodes.InvalidViewDefinition: {
                // The 'DB.prototype.getCollectionInfos()' implementation may throw an exception
                // when faced with a malformed view definition. This is analogous to a missing
                // collection for the purpose of passthrough suites.
                return undefined;
            }
            default:
                throw ex;
        }
    }
}

export function isSystemCollectionName(collectionName) {
    return collectionName.startsWith("system.");
}

export function isInternalDbName(dbName) {
    return ["admin", "local", "config"].includes(dbName);
}

/**
 * Returns true iff the 'collectionName' exists and it is a timeseries collection.
 */
export function isTimeSeriesCollection(db, collectionName) {
    const collectionInfo = db.getCollectionInfos({name: collectionName});
    if (!collectionInfo || collectionInfo.length === 0) {
        return false;
    }
    return collectionInfo[0].type === "timeseries" || collectionName.startsWith("system.bucket.");
}

/**
 * Return true iff this is a "system.bucket.*" collection.
 */
export function isSystemBucketNss(innerCmd) {
    const nss = innerCmd[getCommandName(innerCmd)];
    return typeof nss === "string" && nss.startsWith(sysCollNamePrefix);
}
