// Tests the maxTransactionLockRequestTimeoutMillis server parameter.

import {testNumericServerParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Valid parameter values are in the range (-infinity, infinity).
testNumericServerParameter(
    "maxTransactionLockRequestTimeoutMillis",
    true /*isStartupParameter*/,
    true /*isRuntimeParameter*/,
    5 /*defaultValue*/,
    30 /*nonDefaultValidValue*/,
    false /*hasLowerBound*/,
    "unused" /*lowerOutOfBounds*/,
    false /*hasUpperBound*/,
    "unused" /*upperOutOfBounds*/,
);
