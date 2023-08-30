/**
 * Overrides the runCommand method to append $tenant on command body, so that the requests send by
 * client will pass the tenant information to server.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    assertExpectedDbNameInResponse,
    removeTenantPrefixFromResponse
} from "jstests/libs/override_methods/tenant_aware_response_checker.js";

const kTenantId = ObjectId(TestData.tenantId);

// Override the runCommand to inject $tenant.
function runCommandWithDollarTenant(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    let expectPrefix = false;
    if (TestData.expectPrefix) {
        expectPrefix = TestData.expectPrefix;
        cmdObj = Object.assign(cmdObj, {"expectPrefix": TestData.expectPrefix});
    }

    // inject $tenant to cmdObj.
    const cmdToRun = Object.assign({}, cmdObj, {"$tenant": kTenantId});
    // Actually run the provided command.
    let res = originalRunCommand.apply(conn, makeRunCommandArgs(cmdToRun));

    const prefix = kTenantId + "_";
    const prefixedDbName = prefix + dbName;

    assertExpectedDbNameInResponse(res, dbName, prefixedDbName, tojsononeline(res), expectPrefix);
    removeTenantPrefixFromResponse(res, prefix);
    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_dollar_tenant.js");
OverrideHelpers.overrideRunCommand(runCommandWithDollarTenant);
