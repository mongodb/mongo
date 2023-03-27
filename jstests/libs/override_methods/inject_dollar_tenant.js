/**
 * Overrides the runCommand method to append $tenant on command body, so that the requests send by
 * client will pass the tenant information to server.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

const kTenantId = ObjectId(TestData.tenant);

// Override the runCommand to inject $tenant.
function runCommandWithDollarTenant(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    // inject $tenant to cmdObj.
    const cmdToRun = Object.assign({}, cmdObj, {"$tenant": kTenantId});
    // Actually run the provided command.
    let res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdToRun));
    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_dollar_tenant.js");
OverrideHelpers.overrideRunCommand(runCommandWithDollarTenant);
})();
