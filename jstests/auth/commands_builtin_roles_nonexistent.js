/**
 * Validates the test configuration in commands_builtin_roles.js
 *
 * Makes sure that none of the test cases reference roles
 * that aren't part of the global "roles" array.
 */

import {roles} from "jstests/auth/lib/commands_builtin_roles.js";
import {authCommandsLib} from "jstests/auth/lib/commands_lib.js";

function checkForNonExistentRoles() {
    const tests = authCommandsLib.tests;
    for (let i = 0; i < tests.length; i++) {
        const test = tests[i];
        for (let j = 0; j < test.testcases.length; j++) {
            let testcase = test.testcases[j];
            for (let role in testcase.roles) {
                let roleExists = false;
                for (let k = 0; k < roles.length; k++) {
                    if (roles[k].key === role) {
                        roleExists = true;
                        break;
                    }
                }
                assert(roleExists,
                       "Role " + role + " found in test: " + test.testname +
                           ", but doesn't exist in roles array");
            }
        }
    }
}

checkForNonExistentRoles();
