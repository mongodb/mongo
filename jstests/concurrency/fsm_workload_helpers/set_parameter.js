/**
 * Changes the setParameter on each mongod and/or each mongos for the given cluster.
 *
 * @returns the original value for the given parameter (which can be useful to reset the value after
 *     your workload is finished).
 *
 * @param {Cluster} cluster - The FSM workload cluster - given in setup and teardown.
 * @param {String} paramName - The name of the setParameter to change.
 * @param {any} newValue - The new value to configure for the parameter.
 * @param {Boolean} assertAllSettingsWereIdentical - whether to enforce that the settings were in
 *     sync across all nodes before this change is applied. All settings will be in sync after
 *     changes are applied.
 * @param {Boolean} onMongod - whether to apply to mongod nodes (default: true).
 * @param {Boolean} onMongos - whether to apply to mongos nodes (default: true).
 */
export function setParameterOnAllNodes({
    cluster,
    paramName,
    newValue,
    assertAllSettingsWereIdentical,
    onMongod = true,
    onMongos = true,
}) {
    assert(onMongod || onMongos, "setParameterOnAllNodes requires at least one of onMongod/onMongos to be true");
    let returnValue = null;
    let lastSeenHost = null;
    const setParam = (db) => {
        const res = db.adminCommand({setParameter: 1, [paramName]: newValue});
        assert.commandWorked(res);

        if (returnValue != null && assertAllSettingsWereIdentical) {
            assert.eq(
                returnValue,
                res.was,
                `Expected all hosts to start with the same value for the parameter. Latest is: ${db
                    .getMongo()
                    .toString()}, last seen is: ${lastSeenHost}`,
            );
        }
        lastSeenHost = db.getMongo().toString();
        returnValue = res.was;
    };
    if (onMongod) {
        cluster.executeOnMongodNodes(setParam);
    }
    if (onMongos) {
        cluster.executeOnMongosNodes(setParam);
    }
    return returnValue;
}
