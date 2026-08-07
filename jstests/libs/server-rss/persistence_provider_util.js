import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Utility for persistence provider property checking. Relies on the test-only
 * 'persistenceProviderProperties' command.
 */
export var PersistenceProviderUtil = (function () {
    /**
     * Returns connections to all the nodes of the fixture 'db' belongs to.
     */
    function discoverFixtureNodes(db, fixtureInjectsFailovers) {
        const kMaxDiscoveryRetries = 10;
        const kDiscoveryRetrySleepMS = 1000;

        /**
         * Returns true if 'e' is a replica set host selection failure, which is what topology
         * discovery reports while a just-restarted node has not yet become primary or secondary.
         *
         * Only host selection failures are treated as transient. Network errors are deliberately
         * left out: the constructor for 'ReplSetTest(seedNode)' invoked by getAllNodes() already
         * invokes 'retryOnRetryableError()' internally.
         */
        function isHostSelectionFailure(e) {
            return (
                e.code === ErrorCodes.FailedToSatisfyReadPreference ||
                String(e.message || "").includes("Could not find host matching read preference")
            );
        }

        let attemptsLeft = fixtureInjectsFailovers ? kMaxDiscoveryRetries : 0;
        for (;;) {
            try {
                return FixtureHelpers.getAllNodes(db);
            } catch (e) {
                if (attemptsLeft-- <= 0 || !isHostSelectionFailure(e)) {
                    throw e;
                }
                jsTest.log.info(
                    "persistenceProviderProperties: retrying fixture topology discovery",
                    {error: e.message, attemptsLeft},
                );
                sleep(kDiscoveryRetrySleepMS);
            }
        }
    }

    /**
     * Runs 'persistenceProviderProperties' over a direct connection to 'node' and returns the raw response
     * (or throws an exception if no connection can be established).
     */
    function runPropertiesCommandOnNode(node, fixtureInjectsFailovers) {
        const kMaxNodeRetries = 5;
        const kNodeRetrySleepMS = 1000;

        const runCommand = () =>
            node.getDB("admin").runCommand({
                persistenceProviderProperties: 1,
            });

        // 'retryOnRetryableError()' coerces a retry count of 0 up to 1, so a fixture that does not
        // inject failovers has to bypass it entirely to keep surfacing errors immediately.
        return fixtureInjectsFailovers
            ? retryOnRetryableError(runCommand, kMaxNodeRetries, kNodeRetrySleepMS)
            : runCommand();
    }

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

        const fixtureInjectsFailovers = Boolean(
            TestData.killShards ||
                TestData.runningWithStepdowns ||
                TestData.runningWithShardStepdowns ||
                TestData.runningWithConfigStepdowns,
        );

        // If no nodes are provided, get list of all nodes in the fixture.
        nodes = nodes || discoverFixtureNodes(db, fixtureInjectsFailovers);

        let firstResult = undefined;

        // Helper to access nested properties via dot notation strings.
        function getProperty(obj, path) {
            return path.split(".").reduce((parent, subpath) => parent && parent[subpath], obj);
        }

        for (const node of nodes) {
            let res = runPropertiesCommandOnNode(node, fixtureInjectsFailovers);

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
