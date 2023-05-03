# Upgrade ESLint version

### Bundling ESLint executable

1. Install the latest [Node.js](https://nodejs.org/en/download/) if you don't have it.
2. Install [pkg](https://www.npmjs.com/package/pkg) with npm.
   ```
   npm install -g pkg
   ```
3. Get [ESLint](https://github.com/eslint/eslint) source code.
   ```
   git clone git@github.com:eslint/eslint.git
   ```
4. Checkout the latest version using git tag.
   ```
   cd eslint
   git checkout v${version}
   ```
5. Add pkg options to `package.json` file.
   ```
   "pkg": {
     "scripts": [ "conf/**/*", "lib/**/*", "messages/**/*" ],
     "targets": [ "linux-x64", "macos-x64" ]
     # "targets": [ "linux-arm" ] 
     },
   ```
6. Run pkg command to make ESLint executables.
   ```
   npm install
   pkg .
   ```
7. Check that executables are working.
   Copy files to somewhere in your PATH and try to run it.

   Depending on your system
   ```
   eslint-linux --help
   ```
   or
   ```
   eslint-macos --help
   ```
   or (if you are on arm)
   ```
   eslint --help
   ```

(*) If executable fails to find some .js files there are [extra steps](#extra-steps)
required to be done before step 6.

### Prepare archives

Rename produced files.
```
mv eslint-linux eslint-Linux-x86_64
mv eslint-macos eslint-Darwin-x86_64
# arm
# mv eslint eslint-Linux-arm64
```
Archive files. (No leading v in version e.g. 8.28.0 NOT v8.28.0)
```
tar -czvf eslint-${version}-linux-x86_64.tar.gz eslint-Linux-x86_64
tar -czvf eslint-${version}-darwin.tar.gz eslint-Darwin-x86_64
# arm
# tar -czvf eslint-${version}-linux-arm64.tar.gz eslint-Linux-arm64
```

### Upload archives to `boxes.10gen.com`

Archives should be available by the following links:
```
https://s3.amazonaws.com/boxes.10gen.com/build/eslint-${version}-linux-x86_64.tar.gz
https://s3.amazonaws.com/boxes.10gen.com/build/eslint-${version}-darwin.tar.gz
# arm
# https://s3.amazonaws.com/boxes.10gen.com/build/eslint-${version}-linux-arm64.tar.gz
```
Build team has an access to do that.
You can create a build ticket in Jira for them to do it
(e.g. https://jira.mongodb.org/browse/BUILD-12984)

### Update ESLint version in `buildscripts/eslint.py`
```
# Expected version of ESLint.
ESLINT_VERSION = "${version}"
```

### Extra steps

Unfortunately pkg doesn't work well with `require(variable)` statements
and force include files using `assets` or `scripts` options might not help.

For the ESLint version 7.22.0 and 8.28.0 the following change was applied to the
source code to make everything work:
```
diff --git a/lib/cli-engine/cli-engine.js b/lib/cli-engine/cli-engine.js
index b1befaa04..e02230f83 100644
--- a/lib/cli-engine/cli-engine.js
+++ b/lib/cli-engine/cli-engine.js
@@ -987,43 +987,35 @@ class CLIEngine {
      */
     getFormatter(format) {
 
-        // default is stylish
-        const resolvedFormatName = format || "stylish";
-
-        // only strings are valid formatters
-        if (typeof resolvedFormatName === "string") {
-
-            // replace \ with / for Windows compatibility
-            const normalizedFormatName = resolvedFormatName.replace(/\\/gu, "/");
-
-            const slots = internalSlotsMap.get(this);
-            const cwd = slots ? slots.options.cwd : process.cwd();
-            const namespace = naming.getNamespaceFromTerm(normalizedFormatName);
-
-            let formatterPath;
-
-            // if there's a slash, then it's a file (TODO: this check seems dubious for scoped npm packages)
-            if (!namespace && normalizedFormatName.indexOf("/") > -1) {
-                formatterPath = path.resolve(cwd, normalizedFormatName);
-            } else {
-                try {
-                    const npmFormat = naming.normalizePackageName(normalizedFormatName, "eslint-formatter");
-
-                    formatterPath = ModuleResolver.resolve(npmFormat, path.join(cwd, "__placeholder__.js"));
-                } catch {
-                    formatterPath = path.resolve(__dirname, "formatters", normalizedFormatName);
-                }
-            }
-
-            try {
-                return require(formatterPath);
-            } catch (ex) {
-                ex.message = `There was a problem loading formatter: ${formatterPath}\nError: ${ex.message}`;
-                throw ex;
-            }
-
-        } else {
-            return null;
+        switch (format) {
+            case "checkstyle":
+                return require("./formatters/checkstyle.js");
+            case "codeframe":
+                return require("./formatters/codeframe.js");
+            case "compact":
+                return require("./formatters/compact.js");
+            case "html":
+                return require("./formatters/html.js");
+            case "jslint-xml":
+                return require("./formatters/jslint-xml.js");
+            case "json-with-metadata":
+                return require("./formatters/json-with-metadata.js");
+            case "json":
+                return require("./formatters/json.js");
+            case "junit":
+                return require("./formatters/junit.js");
+            case "stylish":
+                return require("./formatters/stylish.js");
+            case "table":
+                return require("./formatters/table.js");
+            case "tap":
+                return require("./formatters/tap.js");
+            case "unix":
+                return require("./formatters/unix.js");
+            case "visualstudio":
+                return require("./formatters/visualstudio.js");
+            default:
+                return require("./formatters/stylish.js");
         }
     }
 }
```
