// type declarations for mongo.js

declare class Mongo {
    constructor(uri?: string, encryptedDBClientCallback?, options?: object)
    
    getDB(name): DB

    startSession(opts?): DriverSession

    find(ns, query, fields, limit, skip, batchSize, options)
    insert(ns, obj)
    remove(ns, pattern)
    update(ns, query, obj, upsert)
    setSlaveOk(value)
    getSlaveOk()
    setSecondaryOk(value = true)
    getSecondaryOk()
    getDB(name)
    getDBs(driverSession)
    adminCommand(cmd)
    runCommand(dbname, cmd, options)
    getLogComponents(driverSession)
    setLogLevel()
    getDBNames()
    getCollection(ns)
    toString()
    tojson()
    setReadPref(mode, tagSet)
    getReadPrefMode()
    getReadPrefTagSet()
    getReadPref()
    setReadConcern(level)
    getReadConcern()
    setWriteConcern(wc)
    getWriteConcern()
    unsetWriteConcern()
    advanceClusterTime(newTime)
    resetClusterTime_forTesting()
    getClusterTime()
    startSession(options = {})
    isCausalConsistency()
    setCausalConsistency(causalConsistency = true)
    waitForClusterTime(maxRetries = 10)
    watch(pipeline, options)
}
