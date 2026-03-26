import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Utility for persistence provider property checking. Relies on the test-only
 * 'persistenceProviderProperties' command.
 */
export var PersistenceProviderUtil = (function () {
    /**
     * Checks that all nodes in the cluster have the given property with the expected value. It is
     * valid to pass in 'undefined' as 'propertyValue' if the property is expected to be missing
     * on all nodes.
     *
     * The helper relies on the fact that a missing key evaluates to 'undefined' in JavaScript, so
     * any key with an expected value that is not 'undefined', if not present in all of the nodes,
     * will cause the function to return false.
     *
     * Any disagreement between nodes on whether the 'persistenceProviderProperties' command is
     * supported (i.e. some nodes return ok: 1 and some return ok: 0) will also cause the function
     * to return false.
     *
     * The set of nodes to query can optionally be provided via the 'nodes' parameter. If it is not
     * set, all nodes of the current fixture will be queried.
     */
    function allNodesHavePropertyWithValue(dbOrMongo, propertyName, propertyValue, nodes) {
        let db = dbOrMongo;
        if (dbOrMongo instanceof Mongo) {
            db = dbOrMongo.getDB("admin");
        } else if (!(dbOrMongo instanceof DB)) {
            throw new Error("Expected argument to be either a DB or a Mongo instance");
        }

        // If no nodes are provided, get list of all nodes in the fixture.
        nodes = nodes || FixtureHelpers.getAllNodes(db);

        let firstResult = undefined;

        // Helper to access nested properties via dot notation strings.
        function getProperty(obj, path) {
            return path.split(".").reduce((parent, subpath) => parent && parent[subpath], obj);
        }

        for (const node of nodes) {
            const res = node.getDB("admin").runCommand({
                persistenceProviderProperties: 1,
            });

            if (firstResult === undefined) {
                firstResult = res;
            }

            // Early exit if the command was supported in one node but not in another.
            if (res.ok !== firstResult.ok) {
                return false;
            }

            // Current node has different value for the property than expected.
            const currentValue = getProperty(res, propertyName);
            if (bsonUnorderedFieldsCompare(currentValue, propertyValue) !== 0) {
                return false;
            }

            // A previous node disagrees.
            const firstValue = getProperty(firstResult, propertyName);
            if (bsonUnorderedFieldsCompare(firstValue, currentValue) !== 0) {
                return false;
            }
        }

        return true;
    }

    return {
        allNodesHavePropertyWithValue: allNodesHavePropertyWithValue,
    };
})();
