/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const {
  L10nRegistry,
  FileSource,
  IndexedFileSource,
} = ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");
const {setTimeout} = ChromeUtils.import("resource://gre/modules/Timer.jsm");

let fs;

L10nRegistry.loadSync = function(url) {
  if (!fs.hasOwnProperty(url)) {
    return false;
  }
  return fs[url];
};

add_task(function test_methods_presence() {
  equal(typeof L10nRegistry.generateBundles, "function");
  equal(typeof L10nRegistry.getAvailableLocales, "function");
  equal(typeof L10nRegistry.registerSources, "function");
  equal(typeof L10nRegistry.removeSources, "function");
  equal(typeof L10nRegistry.updateSources, "function");
});

/**
 * Test that passing empty resourceIds list works.
 */
add_task(function test_empty_resourceids() {
  fs = {};

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  const bundles = L10nRegistry.generateBundlesSync(["en-US"], []);

  const done = (bundles.next()).done;

  equal(done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * Test that passing empty sources list works.
 */
add_task(function test_empty_sources() {
  fs = {};

  const bundles = L10nRegistry.generateBundlesSync(["en-US"], []);

  const done = (bundles.next()).done;

  equal(done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test tests generation of a proper context for a single
 * source scenario
 */
add_task(function test_methods_calling() {
  fs = {
    "/localization/en-US/browser/menu.ftl": "key = Value",
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  const bundles = L10nRegistry.generateBundlesSync(["en-US"], ["/browser/menu.ftl"]);

  const bundle = (bundles.next()).value;

  equal(bundle.hasMessage("key"), true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that the public methods return expected values
 * for the single source scenario
 */
add_task(function test_has_one_source() {
  let oneSource = new FileSource("app", ["en-US"], "./app/data/locales/{locale}/");
  fs = {
    "./app/data/locales/en-US/test.ftl": "key = value en-US",
  };
  L10nRegistry.registerSources([oneSource]);


  // has one source

  equal(L10nRegistry.sources.size, 1);
  equal(L10nRegistry.sources.has("app"), true);


  // returns a single context

  let bundles = L10nRegistry.generateBundlesSync(["en-US"], ["test.ftl"]);
  let bundle0 = (bundles.next()).value;
  equal(bundle0.hasMessage("key"), true);

  equal((bundles.next()).done, true);


  // returns no contexts for missing locale

  bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);

  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that public methods return expected values
 * for the dual source scenario.
 */
add_task(function test_has_two_sources() {
  let oneSource = new FileSource("platform", ["en-US"], "./platform/data/locales/{locale}/");
  let secondSource = new FileSource("app", ["pl"], "./app/data/locales/{locale}/");
  L10nRegistry.registerSources([oneSource, secondSource]);
  fs = {
    "./platform/data/locales/en-US/test.ftl": "key = platform value",
    "./app/data/locales/pl/test.ftl": "key = app value",
  };


  // has two sources

  equal(L10nRegistry.sources.size, 2);
  equal(L10nRegistry.sources.has("app"), true);
  equal(L10nRegistry.sources.has("platform"), true);


  // returns correct contexts for en-US

  let bundles = L10nRegistry.generateBundlesSync(["en-US"], ["test.ftl"]);
  let bundle0 = (bundles.next()).value;

  equal(bundle0.hasMessage("key"), true);
  let msg = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg.value), "platform value");

  equal((bundles.next()).done, true);


  // returns correct contexts for [pl, en-US]

  bundles = L10nRegistry.generateBundlesSync(["pl", "en-US"], ["test.ftl"]);
  bundle0 = (bundles.next()).value;
  equal(bundle0.locales[0], "pl");
  equal(bundle0.hasMessage("key"), true);
  let msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "app value");

  let bundle1 = (bundles.next()).value;
  equal(bundle1.locales[0], "en-US");
  equal(bundle1.hasMessage("key"), true);
  let msg1 = bundle1.getMessage("key");
  equal(bundle1.formatPattern(msg1.value), "platform value");

  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that behavior specific to the IndexedFileSource
 * works correctly.
 *
 * In particular it tests that IndexedFileSource correctly returns
 * missing files as `false` instead of `undefined`.
 */
add_task(function test_indexed() {
  let oneSource = new IndexedFileSource("langpack-pl", ["pl"], "/data/locales/{locale}/", [
    "/data/locales/pl/test.ftl",
  ]);
  L10nRegistry.registerSources([oneSource]);
  fs = {
    "/data/locales/pl/test.ftl": "key = value",
  };

  equal(L10nRegistry.sources.size, 1);
  equal(L10nRegistry.sources.has("langpack-pl"), true);

  equal(oneSource.getPath("pl", "test.ftl"), "/data/locales/pl/test.ftl");
  equal(oneSource.hasFile("pl", "test.ftl"), true);
  equal(oneSource.hasFile("pl", "missing.ftl"), false);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test checks if the correct order of contexts is used for
 * scenarios where a new file source is added on top of the default one.
 */
add_task(function test_override() {
  let fileSource = new FileSource("app", ["pl"], "/app/data/locales/{locale}/");
  let oneSource = new IndexedFileSource("langpack-pl", ["pl"], "/data/locales/{locale}/", [
    "/data/locales/pl/test.ftl",
  ]);
  L10nRegistry.registerSources([fileSource, oneSource]);

  fs = {
    "/app/data/locales/pl/test.ftl": "key = value",
    "/data/locales/pl/test.ftl": "key = addon value",
  };

  equal(L10nRegistry.sources.size, 2);
  equal(L10nRegistry.sources.has("langpack-pl"), true);

  let bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  let bundle0 = (bundles.next()).value;
  equal(bundle0.locales[0], "pl");
  equal(bundle0.hasMessage("key"), true);
  let msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "addon value");

  let bundle1 = (bundles.next()).value;
  equal(bundle1.locales[0], "pl");
  equal(bundle1.hasMessage("key"), true);
  let msg1 = bundle1.getMessage("key");
  equal(bundle1.formatPattern(msg1.value), "value");

  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that new contexts are returned
 * after source update.
 */
add_task(function test_updating() {
  let oneSource = new IndexedFileSource("langpack-pl", ["pl"], "/data/locales/{locale}/", [
    "/data/locales/pl/test.ftl",
  ]);
  L10nRegistry.registerSources([oneSource]);
  fs = {
    "/data/locales/pl/test.ftl": "key = value",
  };

  let bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  let bundle0 = (bundles.next()).value;
  equal(bundle0.locales[0], "pl");
  equal(bundle0.hasMessage("key"), true);
  let msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "value");


  const newSource = new IndexedFileSource("langpack-pl", ["pl"], "/data/locales/{locale}/", [
    "/data/locales/pl/test.ftl",
  ]);
  fs["/data/locales/pl/test.ftl"] = "key = new value";
  L10nRegistry.updateSources([newSource]);

  equal(L10nRegistry.sources.size, 1);
  bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  bundle0 = (bundles.next()).value;
  msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "new value");

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that generated contexts return correct values
 * after sources are being removed.
 */
add_task(function test_removing() {
  let fileSource = new FileSource("app", ["pl"], "/app/data/locales/{locale}/");
  let oneSource = new IndexedFileSource("langpack-pl", ["pl"], "/data/locales/{locale}/", [
    "/data/locales/pl/test.ftl",
  ]);
  L10nRegistry.registerSources([fileSource, oneSource]);

  fs = {
    "/app/data/locales/pl/test.ftl": "key = value",
    "/data/locales/pl/test.ftl": "key = addon value",
  };

  equal(L10nRegistry.sources.size, 2);
  equal(L10nRegistry.sources.has("langpack-pl"), true);

  let bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  let bundle0 = (bundles.next()).value;
  equal(bundle0.locales[0], "pl");
  equal(bundle0.hasMessage("key"), true);
  let msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "addon value");

  let bundle1 = (bundles.next()).value;
  equal(bundle1.locales[0], "pl");
  equal(bundle1.hasMessage("key"), true);
  let msg1 = bundle1.getMessage("key");
  equal(bundle1.formatPattern(msg1.value), "value");

  equal((bundles.next()).done, true);

  // Remove langpack

  L10nRegistry.removeSources(["langpack-pl"]);

  equal(L10nRegistry.sources.size, 1);
  equal(L10nRegistry.sources.has("langpack-pl"), false);

  bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  bundle0 = (bundles.next()).value;
  equal(bundle0.locales[0], "pl");
  equal(bundle0.hasMessage("key"), true);
  msg0 = bundle0.getMessage("key");
  equal(bundle0.formatPattern(msg0.value), "value");

  equal((bundles.next()).done, true);

  // Remove app source

  L10nRegistry.removeSources(["app"]);

  equal(L10nRegistry.sources.size, 0);

  bundles = L10nRegistry.generateBundlesSync(["pl"], ["test.ftl"]);
  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that the logic works correctly when there's a missing
 * file in the FileSource scenario.
 */
add_task(function test_missing_file() {
  let oneSource = new FileSource("app", ["en-US"], "./app/data/locales/{locale}/");
  let twoSource = new FileSource("platform", ["en-US"], "./platform/data/locales/{locale}/");
  L10nRegistry.registerSources([oneSource, twoSource]);

  fs = {
    "./app/data/locales/en-US/test.ftl": "key = value en-US",
    "./platform/data/locales/en-US/test.ftl": "key = value en-US",
    "./platform/data/locales/en-US/test2.ftl": "key2 = value2 en-US",
  };


  // has two sources

  equal(L10nRegistry.sources.size, 2);
  equal(L10nRegistry.sources.has("app"), true);
  equal(L10nRegistry.sources.has("platform"), true);


  // returns a single context

  let bundles = L10nRegistry.generateBundlesSync(["en-US"], ["test.ftl", "test2.ftl"]);

  // First permutation:
  //   [platform, platform] - both present
  let bundle1 = (bundles.next());
  equal(bundle1.value.hasMessage("key"), true);

  // Second permutation skipped:
  //   [platform, app] - second missing
  // Third permutation:
  //   [app, platform] - both present
  let bundle2 = (bundles.next());
  equal(bundle2.value.hasMessage("key"), true);

  // Fourth permutation skipped:
  //   [app, app] - second missing
  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});

/**
 * This test verifies that each file is that all files requested
 * by a single context are fetched at the same time, even
 * if one I/O is slow.
 */
add_task(function test_parallel_io() {
  /* eslint-disable mozilla/no-arbitrary-setTimeout */
  let originalLoad = L10nRegistry.load;
  let fetchIndex = new Map();

  L10nRegistry.load = function(url) {
    if (!fetchIndex.has(url)) {
      fetchIndex.set(url, 0);
    }
    fetchIndex.set(url, fetchIndex.get(url) + 1);

    if (url === "/en-US/slow-file.ftl") {
      return new Promise((resolve, reject) => {
        setTimeout(() => {
          // Despite slow-file being the first on the list,
          // by the time the it finishes loading, the other
          // two files are already fetched.
          equal(fetchIndex.get("/en-US/test.ftl"), 1);
          equal(fetchIndex.get("/en-US/test2.ftl"), 1);

          resolve("");
        }, 10);
      });
    }
    return Promise.resolve("");
  };
  let oneSource = new FileSource("app", ["en-US"], "/{locale}/");
  L10nRegistry.registerSources([oneSource]);

  fs = {
    "/en-US/test.ftl": "key = value en-US",
    "/en-US/test2.ftl": "key2 = value2 en-US",
    "/en-US/slow-file.ftl": "key-slow = value slow en-US",
  };

  // returns a single context

  let bundles = L10nRegistry.generateBundlesSync(["en-US"], ["slow-file.ftl", "test.ftl", "test2.ftl"]);

  equal(fetchIndex.size, 0);

  let bundle0 = bundles.next();

  equal(bundle0.done, false);

  equal((bundles.next()).done, true);

  // When requested again, the cache should make the load operation not
  // increase the fetchedIndex count
  L10nRegistry.generateBundlesSync(["en-US"], ["test.ftl", "test2.ftl", "slow-file.ftl"]);

  // cleanup
  L10nRegistry.sources.clear();
  L10nRegistry.load = originalLoad;
});

/**
 * This test verifies that we handle correctly a scenario where a source
 * is being removed while the iterator operates.
 */
add_task(function test_remove_source_mid_iter_cycle() {
  let oneSource = new FileSource("platform", ["en-US"], "./platform/data/locales/{locale}/");
  let secondSource = new FileSource("app", ["pl"], "./app/data/locales/{locale}/");
  L10nRegistry.registerSources([oneSource, secondSource]);

  fs = {
    "./platform/data/locales/en-US/test.ftl": "key = platform value",
    "./app/data/locales/pl/test.ftl": "key = app value",
  };

  let bundles = L10nRegistry.generateBundlesSync(["en-US", "pl"], ["test.ftl"]);

  let bundle0 = bundles.next();

  L10nRegistry.removeSources(["app"]);

  equal((bundles.next()).done, true);

  // cleanup
  L10nRegistry.sources.clear();
});
