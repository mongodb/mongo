// type declarations for global.h

declare var buildInfo;
declare function gc();
declare function getJSHeapLimitMB();
declare function print();
declare function sleep();
declare function version();

type InternalModuleName = "performance";

/**
 * Loads a shell-internal module binding for std module bootstrapping.
 *
 * This API is intended only for implementations of `std:*` modules under
 * `src/mongo/shell/std`. Calling this from non-`std:*` modules throws at
 * runtime.
 *
 * JSTests must not call this directly. Instead import the std module:
 * e.g.`import {performance} from "std:performance";`
 */
declare function internalModule(
    moduleName: InternalModuleName,
): Record<string, unknown>;
