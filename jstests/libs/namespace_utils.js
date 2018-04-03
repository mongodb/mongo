/**
 * Returns the collection name extracted from a namespace string.
 */
function getCollectionNameFromFullNamespace(ns) {
    return ns.split(/\.(.+)/)[1];
}
