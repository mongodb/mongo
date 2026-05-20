# `std` Modules

This directory contains shell standard modules that are imported as `std:<name>` from jstests.

Example:

```js
import {performance} from "std:performance";
```

## What are `std` modules

A `std` module is a built-in module shipped with the `mongo` test runner. Each module is comprised
of:

- a public JavaScript module file (`<name>.js`) which defines the public API of the module.
- an internal C++ binding implementation (`<name>.cpp`) offering bindings between C++ and
  JavaScript.
- a TypeScript declaration file for the public API (`<name>.d.ts`), documenting the API for editor
  discoverability.

## What is `internalModule`

The C++ bindings for internal modules declare methods and class that can be used for interoperate
between the two environments. These bindings are exclusively limited to use in the public JavaScript
API file, so as to not pollute the global namespace. This helps us localize documentation and
improve discoverability.

## How to Define a New `std` Module

When adding a new module `<name>`, update all of the following:

1. Add public API file: `src/mongo/shell/std/<name>.js`

- Export the stable API users import from `std:<name>`.
- Use `internalModule("<name>")` only inside this `std:*` module implementation.

2. Add internal binding file: `src/mongo/shell/std/<name>.cpp`

- Define an initializer with signature `bool init(JSContext*, JS::HandleObject target)`.
- Add functions/properties onto `target`.
- Register with
  `MONGO_REGISTER_INTERNAL_MODULE_WITH_SETUP("<name>", initFn, &::mongo::JSFiles::std_<name>)`.

3. Add public typings: `src/mongo/shell/std/<name>.d.ts`

- Describe exported symbols from `<name>.js`.

4. Register the std module name in `src/mongo/shell/BUILD.bazel`

- Add `("std:<name>", "std/<name>.js")` to `MONGOJS_STD_JS_MODULES`.

5. Update allowed internal module names in `src/mongo/scripting/mozjs/common/global.d.ts`

- Add `"<name>"` to the `InternalModuleName` set used by `internalModule()`.

Name consistency matters:

- JS import name uses `std:<name>`
- internal binding lookup uses `<name>` (without the `std:` prefix)
- all entries above should refer to the same module concept

## How the Implementation Works Internally

### Build-time wiring

1. `src/mongo/shell/BUILD.bazel` lists `MONGOJS_STD_JS_MODULES`.
2. `buildscripts/jstoh.py` consumes these entries via `--module` and generates embedded `JSFile`
   entries in `mongojs.cpp`.
3. `std_internal_modules` compiles all `std/*.cpp` files and links them with the internal module
   registry.

### Runtime wiring

1. Each `std/<name>.cpp` registration macro creates a static `InternalModuleRegistrar`.
2. Registrars populate the internal module registry (`internal_module_registry.*`) at startup.
3. `ModuleLoader::init()` defines a global `internalModule()` function and calls
   `preloadInternalModules()`.
4. `preloadInternalModules()`:

- creates a binding object per registered internal module
- calls each module's C++ initializer to populate it
- stores the binding in an internal registry map keyed by module name
- loads the module setup JS (`std:<name>`) from embedded `JSFile` source

5. The setup JS module (for example `std/performance.js`) runs as a `std:*` module and is allowed to
   call `internalModule("<name>")`.
6. Non-`std:*` callers are rejected by `internalModule()` at runtime.

This split keeps native internals private while exposing a stable, documented JavaScript API.
