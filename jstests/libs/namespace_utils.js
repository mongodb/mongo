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
