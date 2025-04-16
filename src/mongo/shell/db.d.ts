// type declarations for db.js

declare class DB {

    constructor(mongo: Mongo, name: string);

    getMongo(): Mongo
    getName(): string

    /**
     * Dynamic property to produce a DBCollection.
     * 
     * @example
     * let coll1 = db[jsTestName()];
     * let coll2 = db.anotherCollectionUnderTest;
     */
    [collectionIndex: string]: DBCollection

    rotateCertificates(message)
    getSiblingDB(name): DB
    stats(opt)
    getCollection(name): DBCollection
    commandHelp(name)
    runReadCommand()
    runCommand(obj, extra, queryOptions)
    adminCommand(obj, extra)
    aggregate(pipeline, aggregateOptions)
    createCollection(name, opt)
    createView(name, viewOn, pipeline, opt)
    getProfilingLevel()
    getProfilingStatus()
    dropDatabase(writeConcern)
    shutdownServer(opts)
    help()
    printCollectionStats(scale)
    setProfilingLevel(level, options)
    eval(jsfunction)
    groupeval(parmsObj)
    forceError()
    getCollectionNames()
    tojson()
    toString()
    isMaster()
    hello()
    currentOp(arg)
    currentOpCursor(arg)
    killOp(op)
    getReplicationInfo()
    printReplicationInfo()
    printSlaveReplicationInfo()
    printSecondaryReplicationInfo()
    serverBuildInfo()
    serverStatus(options)
    hostInfo()
    serverCmdLineOpts()
    version()
    serverBits()
    listCommands()
    printShardingStatus(verbose)
    fsyncLock()
    fsyncUnlock()
    setSlaveOk(value = true)
    getSlaveOk()
    setSecondaryOk(value = true)
    getSecondaryOk()
    getQueryOptions()
    loadServerScripts()
    createUser(userObj, writeConcern)
    updateUser(name, updateObject, writeConcern)
    changeUserPassword(username, password, writeConcern)
    logout()
    removeUser(username, writeConcern)
    dropUser(username, writeConcern)
    dropAllUsers(writeConcern)
    auth()
    grantRolesToUser(username, roles, writeConcern)
    revokeRolesFromUser(username, roles, writeConcern)
    getUser(username, args)
    getUsers(args)
    createRole(roleObj, writeConcern)
    updateRole(name, updateObject, writeConcern)
    dropRole(name, writeConcern)
    dropAllRoles(writeConcern)
    grantRolesToRole(rolename, roles, writeConcern)
    revokeRolesFromRole(rolename, roles, writeConcern)
    grantPrivilegesToRole(rolename, privileges, writeConcern)
    revokePrivilegesFromRole(rolename, privileges, writeConcern)
    getRole(rolename, args)
    getRoles(args)
    setWriteConcern(wc)
    getWriteConcern()
    unsetWriteConcern()
    getLogComponents()
    setLogLevel(logLevel, component)
    watch(pipeline, options)
    getSession()
    createEncryptedCollection(name, opts)
    dropEncryptedCollection(name)
    checkMetadataConsistency(options = {})
    getDatabasePrimaryShardId()
    getServerBuildInfo()
}
