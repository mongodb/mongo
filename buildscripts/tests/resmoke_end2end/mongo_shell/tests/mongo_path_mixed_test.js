/**
 * Test mixing MONGO_PATH and relative path imports
 */

// Import from MONGO_PATH (short name)
import {TOP_LEVEL_VALUE, getTopLevelValue} from "top_level_module.js";

print("Testing mixed path imports...");
assert.eq(TOP_LEVEL_VALUE, "top_level_import_success", "MONGO_PATH import failed");
print("SUCCESS: MONGO_PATH import worked");

// Load from relative path (from repo root)
load("buildscripts/tests/resmoke_end2end/mongo_shell/modules/common_module.js");
assert.eq(MONGO_PATH_TEST_COMMON, "loaded_from_mongo_path", "Relative path load() failed");
print("SUCCESS: relative path load() worked");

// Dynamic import from MONGO_PATH
const esModule = await import("es_module.js");
assert.eq(esModule.ES_MODULE_VALUE, "imported_from_mongo_path", "MONGO_PATH dynamic import failed");
print("SUCCESS: MONGO_PATH dynamic import() worked");

print("All mixed path tests passed!");
