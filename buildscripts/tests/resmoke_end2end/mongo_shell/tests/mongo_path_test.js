/**
 * Test that MONGO_PATH works for both load() and import()
 */

// Test 0: Top-level import should find files in MONGO_PATH
import {TOP_LEVEL_VALUE, getTopLevelValue} from "top_level_module.js";

// Verify top-level import worked
print("Testing top-level import with MONGO_PATH...");
assert.eq(TOP_LEVEL_VALUE, "top_level_import_success", "Top-level import() failed to load module from MONGO_PATH");
assert.eq(getTopLevelValue(), "top_level_function_works", "Function from top-level imported module doesn't work");
print("SUCCESS: top-level import found file in MONGO_PATH");

// Test 1: load() should find files in MONGO_PATH
print("Testing load() with MONGO_PATH...");
load("common_module.js");
assert.eq(MONGO_PATH_TEST_COMMON, "loaded_from_mongo_path", "load() failed to load module from MONGO_PATH");
assert.eq(getCommonValue(), "common_value_from_load", "Function from loaded module doesn't work");
print("SUCCESS: load() found file in MONGO_PATH");

// Test 2: Dynamic import() should find files in MONGO_PATH
print("Testing dynamic import() with MONGO_PATH...");
const esModule = await import("es_module.js");
assert.eq(esModule.ES_MODULE_VALUE, "imported_from_mongo_path", "import() failed to load module from MONGO_PATH");
assert.eq(esModule.getModuleValue(), "module_value_from_import", "Function from imported module doesn't work");
print("SUCCESS: dynamic import() found file in MONGO_PATH");

print("All MONGO_PATH tests passed (top-level import, load, and dynamic import)!");
