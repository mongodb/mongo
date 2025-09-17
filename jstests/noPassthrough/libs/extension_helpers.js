/**
 * Helpers for generating and deleting extension .conf files in noPassthrough tests.
 */
import {getPython3Binary} from "jstests/libs/python.js";

/**
 * @param {string|string[]} paths A path string or a list of path strings to .so files to
 *    generate .conf files for.
 */
export function generateExtensionConfigs(paths) {
    if (!Array.isArray(paths)) {
        paths = [paths];
    }

    // A hash is appended to the end of the configuration file name to avoid collisions
    // across concurrent tests using the same extension.
    const hash = UUID().hex();
    const ret = runNonMongoProgram(
        getPython3Binary(),
        "-m",
        "buildscripts.resmokelib.extensions.generate_extension_configs",
        "--so-files",
        paths.join(","),
        "--with-suffix",
        hash,
    );
    assert.eq(ret, 0, "Failed to generate extension .conf files");

    for (let i = 0; i < paths.length; i++) {
        const lastDot = paths[i].lastIndexOf(".");
        const basePath = paths[i].substring(0, lastDot);
        const fileExtension = paths[i].substring(lastDot);
        paths[i] = `${basePath}_${hash}${fileExtension}`;
    }

    return paths;
}

export function deleteExtensionConfigs(paths) {
    const ret = runNonMongoProgram(
        getPython3Binary(),
        "-m",
        "buildscripts.resmokelib.extensions.delete_extension_configs",
        "--extension-paths",
        paths.join(","),
    );
    assert.eq(ret, 0, "Failed to delete extension .conf files");
}
