/**
 * Test that modules can be loaded from relative paths without MONGO_PATH
 */

// Test with full relative paths from repo root (not using MONGO_PATH)
import {
    TOP_LEVEL_VALUE,
    getTopLevelValue,
} from "buildscripts/tests/resmoke_end2end/mongo_shell/modules/top_level_module.js";

print("Testing relative path imports...");
assert.eq(TOP_LEVEL_VALUE, "top_level_import_success", "Relative path import failed");
assert.eq(getTopLevelValue(), "top_level_function_works", "Function from relative import doesn't work");
print("SUCCESS: relative path import worked");

// Load using relative path
load("buildscripts/tests/resmoke_end2end/mongo_shell/modules/common_module.js");
assert.eq(MONGO_PATH_TEST_COMMON, "loaded_from_mongo_path", "load() with relative path failed");
assert.eq(getCommonValue(), "common_value_from_load", "Function from relative load doesn't work");
print("SUCCESS: relative path load() worked");

// Dynamic import using relative path
const esModule = await import("buildscripts/tests/resmoke_end2end/mongo_shell/modules/es_module.js");
assert.eq(esModule.ES_MODULE_VALUE, "imported_from_mongo_path", "Dynamic import with relative path failed");
assert.eq(esModule.getModuleValue(), "module_value_from_import", "Function from relative dynamic import doesn't work");
print("SUCCESS: relative path dynamic import() worked");

print("All relative path tests passed!");
