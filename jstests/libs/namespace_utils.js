/**
 * Returns the collection name extracted from a namespace string.
 */
export function getCollectionNameFromFullNamespace(ns) {
    return ns.split(/\.(.+)/)[1];
}

/**
 * Returns the database and collection name extracted from a namespace string.
 */
export function getDBNameAndCollNameFromFullNamespace(ns) {
    return ns.split(/\.(.+)/);
}

/**
 * Returns whether the collection name is valid. See NamespaceString::validCollectionName().
 */
export function isValidCollectionName(collName) {
    if (typeof collName !== "string" || collName.length === 0) {
        return false;
    }
    if (collName.includes("\0")) {
        return false;
    }
    if (collName === "oplog.$main") {
        return true;
    }
    return !collName.startsWith(".") && !collName.includes("$");
}
