/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const { AppConstants } = ChromeUtils.import("resource://gre/modules/AppConstants.jsm");
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

add_task(function test_methods_presence() {
  strictEqual(typeof Localization.prototype.formatValues, "function");
  strictEqual(typeof Localization.prototype.formatMessages, "function");
  strictEqual(typeof Localization.prototype.formatValue, "function");
});

add_task(async function test_methods_calling() {
  const { L10nRegistry, FileSource } =
    ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");

  const fs = {
    "/localization/de/browser/menu.ftl": `
key-value1 = [de] Value2
`,
    "/localization/en-US/browser/menu.ftl": `
key-value1 = [en] Value2
key-value2 = [en] Value3
key-attr =
    .label = [en] Label 3
`,
  };
  const originalLoad = L10nRegistry.load;
  const originalRequested = Services.locale.requestedLocales;

  L10nRegistry.load = async function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["de", "en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  async function* generateBundles(resIds) {
    yield * await L10nRegistry.generateBundles(["de", "en-US"], resIds);
  }

  const l10n = new Localization([
    "/browser/menu.ftl",
  ], false, { generateBundles });

  {
    let values = await l10n.formatValues([
      {id: "key-value1"},
      {id: "key-value2"},
      {id: "key-missing"},
      {id: "key-attr"}
    ]);

    strictEqual(values[0], "[de] Value2");
    strictEqual(values[1], "[en] Value3");
    strictEqual(values[2], null);
    strictEqual(values[3], null);
  }

  {
    let values = await l10n.formatValues([
      "key-value1",
      "key-value2",
      "key-missing",
      "key-attr"
    ]);

    strictEqual(values[0], "[de] Value2");
    strictEqual(values[1], "[en] Value3");
    strictEqual(values[2], null);
    strictEqual(values[3], null);
  }

  {
    strictEqual(await l10n.formatValue("key-missing"), null);
    strictEqual(await l10n.formatValue("key-value1"), "[de] Value2");
    strictEqual(await l10n.formatValue("key-value2"), "[en] Value3");
    strictEqual(await l10n.formatValue("key-attr"), null);
  }

  {
    let messages = await l10n.formatMessages([
      {id: "key-value1"},
      {id: "key-missing"},
      {id: "key-value2"},
      {id: "key-attr"},
    ]);

    strictEqual(messages[0].value, "[de] Value2");
    strictEqual(messages[1], null);
    strictEqual(messages[2].value, "[en] Value3");
    strictEqual(messages[3].value, null);
  }

  L10nRegistry.sources.clear();
  L10nRegistry.load = originalLoad;
  Services.locale.requestedLocales = originalRequested;
});

add_task(async function test_builtins() {
  const { L10nRegistry, FileSource } =
    ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");

  const known_platforms = {
    "linux": "linux",
    "win": "windows",
    "macosx": "macos",
    "android": "android",
  };

  const fs = {
    "/localization/en-US/test.ftl": `
key = { PLATFORM() ->
        ${ Object.values(known_platforms).map(
              name => `      [${ name }] ${ name.toUpperCase() } Value\n`).join("") }
       *[other] OTHER Value
    }`,
  };
  const originalLoad = L10nRegistry.load;

  L10nRegistry.load = async function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  async function* generateBundles(resIds) {
    yield * await L10nRegistry.generateBundles(["en-US"], resIds);
  }

  const l10n = new Localization([
    "/test.ftl",
  ], false, { generateBundles });

  let values = await l10n.formatValues([{id: "key"}]);

  ok(values[0].includes(
    `${ known_platforms[AppConstants.platform].toUpperCase() } Value`));

  L10nRegistry.sources.clear();
  L10nRegistry.load = originalLoad;
});

add_task(async function test_add_remove_resourceIds() {
  const { L10nRegistry, FileSource } =
    ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");

  const fs = {
    "/localization/en-US/browser/menu.ftl": "key1 = Value1",
    "/localization/en-US/toolkit/menu.ftl": "key2 = Value2",
  };
  const originalLoad = L10nRegistry.load;
  const originalRequested = Services.locale.requestedLocales;

  L10nRegistry.load = async function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  async function* generateBundles(resIds) {
    yield * await L10nRegistry.generateBundles(["en-US"], resIds);
  }

  const l10n = new Localization(["/browser/menu.ftl"], false, { generateBundles });

  let values = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], null);

  l10n.addResourceIds(["/toolkit/menu.ftl"]);

  values = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  values = await l10n.formatValues(["key1", {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  values = await l10n.formatValues([{id: "key1"}, "key2"]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  l10n.removeResourceIds(["/browser/menu.ftl"]);

  values = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], null);
  strictEqual(values[1], "Value2");

  L10nRegistry.sources.clear();
  L10nRegistry.load = originalLoad;
  Services.locale.requestedLocales = originalRequested;
});

add_task(async function test_switch_to_async() {
  const { L10nRegistry, FileSource } =
    ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");

  const fs = {
    "/localization/en-US/browser/menu.ftl": "key1 = Value1",
    "/localization/en-US/toolkit/menu.ftl": "key2 = Value2",
  };
  const originalLoad = L10nRegistry.load;
  const originalLoadSync = L10nRegistry.loadSync;
  const originalRequested = Services.locale.requestedLocales;

  let syncLoads = 0;
  let asyncLoads = 0;

  L10nRegistry.load = async function(url) {
    asyncLoads += 1;
    return fs[url];
  };

  L10nRegistry.loadSync = function(url) {
    syncLoads += 1;
    return fs[url];
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  async function* generateBundles(resIds) {
    yield * await L10nRegistry.generateBundles(["en-US"], resIds);
  }

  function* generateBundlesSync(resIds) {
    yield * L10nRegistry.generateBundlesSync(["en-US"], resIds);
  }

  const l10n = new Localization(["/browser/menu.ftl"], false, { generateBundles, generateBundlesSync });

  let values = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], null);
  strictEqual(syncLoads, 0);
  strictEqual(asyncLoads, 1);

  l10n.setIsSync(true);

  l10n.addResourceIds(["/toolkit/menu.ftl"]);

  // Nothing happens when we switch, because
  // the next load is lazy.
  strictEqual(syncLoads, 0);
  strictEqual(asyncLoads, 1);

  values = l10n.formatValuesSync([{id: "key1"}, {id: "key2"}]);
  let values2 = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  deepEqual(values, values2);
  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");
  strictEqual(syncLoads, 1);
  strictEqual(asyncLoads, 1);

  l10n.removeResourceIds(["/browser/menu.ftl"]);

  values = await l10n.formatValues([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], null);
  strictEqual(values[1], "Value2");
  strictEqual(syncLoads, 1);
  strictEqual(asyncLoads, 1);

  L10nRegistry.sources.clear();
  L10nRegistry.load = originalLoad;
  L10nRegistry.loadSync = originalLoadSync;
  Services.locale.requestedLocales = originalRequested;
});
