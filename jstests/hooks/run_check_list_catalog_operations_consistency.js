import {
    assertCatalogListOperationsConsistencyForDb
} from "jstests/libs/catalog_list_operations_consistency_validator.js";

var startTime = Date.now();
assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

const multitenancyRes = db.adminCommand({getParameter: 1, multitenancySupport: 1});
const multitenancy = multitenancyRes.ok && multitenancyRes["multitenancySupport"];
const cmdObj = multitenancy ? {listDatabasesForAllTenants: 1} : {listDatabases: 1};
const dbs = assert.commandWorked(db.adminCommand(cmdObj)).databases;

for (const dbRes of dbs) {
    try {
        const token = dbRes.tenantId ? _createTenantToken({tenant: dbRes.tenantId}) : undefined;
        db.getMongo()._setSecurityToken(token);

        assertCatalogListOperationsConsistencyForDb(db.getSiblingDB(dbRes.name), dbRes.tenantId);
    } finally {
        db.getMongo()._setSecurityToken(undefined);
    }
}

var totalTime = Date.now() - startTime;
print('Finished catalog operations consistency check in ' + totalTime + ' ms.');
