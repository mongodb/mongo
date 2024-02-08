
/**
 * Run the database command using the provided security token on behalf of a tenant.
 * @param {object} tenantToken The provided security token which indicates the tenant.
 * @param {DB} db The database object.
 * @param {JSON} command The command body to be executed.
 * @returns Return a response document.
 */
export function runCommandWithSecurityToken(tenantToken, db, command) {
    const conn = db.getMongo();
    const preToken = conn._securityToken;
    try {
        conn._setSecurityToken(tenantToken);
        return db.runCommand(command);
    } finally {
        conn._setSecurityToken(preToken);
    }
}

/**
 * Make an unsigned security token if the tenant id is provided.
 * @param {ObjectId} tenantId The tenant id to build an unsigned security token for a tenant.
 * @param {object} options The options to indicate the value of expectPrefix field in the security
 *     token.
 * @returns Return an unsigned security token.
 */
export function makeUnsignedSecurityToken(tenantId, options) {
    options = options || {expectPrefix: false};
    const expectPrefix = !!options.expectPrefix;
    return _createTenantToken({tenant: tenantId, expectPrefix: expectPrefix});
}