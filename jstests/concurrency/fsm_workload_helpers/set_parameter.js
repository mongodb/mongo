/**
 * Changes the setParameter on each mongod and on each mongos (if any) for the given cluster.
 *
 * @returns the original value for the given parameter (which can be useful to reset the value after
 *     your workload is finshed).
 *
 * @param {Cluster} cluster - The FSM workload cluster - given in setup and teardown.
 * @param {String} paramName - The name of the setParameter to change.
 * @param {any} newValue - The new value to configure for the parameter.
 * @param {Boolean} assertAllSettingsWereIdentical - whether to enforce that the settings were in
 *     sync across all nodes before this change is applied. All settings will be in sync after
 *     changes are applied.
 */
export function setParameterOnAllNodes(
    {cluster, paramName, newValue, assertAllSettingsWereIdentical}) {
    let returnValue = null;
    let lastSeenHost = null;
    const setQueryStatsParams = (db) => {
        const res = db.adminCommand({setParameter: 1, [paramName]: newValue});
        assert.commandWorked(res);

        if (returnValue != null && assertAllSettingsWereIdentical) {
            assert.eq(
                returnValue,
                res.was,
                `Expected all hosts to start with the same value for the parameter. Latest is: ${
                    db.getMongo().toString()}, last seen is: ${lastSeenHost}`);
        }
        returnValue = res.was;
    };
    cluster.executeOnMongodNodes(setQueryStatsParams);
    cluster.executeOnMongosNodes(setQueryStatsParams);
    return returnValue;
}
