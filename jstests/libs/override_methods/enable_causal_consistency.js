/**
 * Enables causal consistency on the connections.
 */
import "jstests/libs/override_methods/set_read_preference_secondary.js";

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

db.getMongo().setCausalConsistency();

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/enable_causal_consistency.js");
