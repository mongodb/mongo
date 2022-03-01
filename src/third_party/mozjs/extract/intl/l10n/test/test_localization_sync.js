/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const { AppConstants } = ChromeUtils.import("resource://gre/modules/AppConstants.jsm");
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

add_task(function test_methods_calling() {
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
  const originalLoadSync = L10nRegistry.loadSync;
  const originalRequested = Services.locale.requestedLocales;

  L10nRegistry.loadSync = function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["de", "en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  function* generateBundlesSync(resIds) {
    yield * L10nRegistry.generateBundlesSync(["de", "en-US"], resIds);
  }

  const l10n = new Localization([
    "/browser/menu.ftl",
  ], true, { generateBundlesSync });


  {
    let values = l10n.formatValuesSync([
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
    let values = l10n.formatValuesSync([
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
    strictEqual(l10n.formatValueSync("key-missing"), null);
    strictEqual(l10n.formatValueSync("key-value1"), "[de] Value2");
    strictEqual(l10n.formatValueSync("key-value2"), "[en] Value3");
    strictEqual(l10n.formatValueSync("key-attr"), null);
  }

  {
    let messages = l10n.formatMessagesSync([
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
  L10nRegistry.loadSync = originalLoadSync;
  Services.locale.requestedLocales = originalRequested;
});

add_task(function test_builtins() {
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
  const originalLoadSync = L10nRegistry.loadSync;

  L10nRegistry.loadSync = function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  function* generateBundlesSync(resIds) {
    yield * L10nRegistry.generateBundlesSync(["en-US"], resIds);
  }

  const l10n = new Localization([
    "/test.ftl",
  ], true, { generateBundlesSync });

  let values = l10n.formatValuesSync([{id: "key"}]);

  ok(values[0].includes(
    `${ known_platforms[AppConstants.platform].toUpperCase() } Value`));

  L10nRegistry.sources.clear();
  L10nRegistry.loadSync = originalLoadSync;
});

add_task(function test_add_remove_resourceIds() {
  const { L10nRegistry, FileSource } =
    ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");

  const fs = {
    "/localization/en-US/browser/menu.ftl": "key1 = Value1",
    "/localization/en-US/toolkit/menu.ftl": "key2 = Value2",
  };
  const originalLoadSync = L10nRegistry.loadSYnc;
  const originalRequested = Services.locale.requestedLocales;

  L10nRegistry.loadSync = function(url) {
    return fs[url];
  };

  const source = new FileSource("test", ["en-US"], "/localization/{locale}");
  L10nRegistry.registerSources([source]);

  function* generateBundlesSync(resIds) {
    yield * L10nRegistry.generateBundlesSync(["en-US"], resIds);
  }

  const l10n = new Localization(["/browser/menu.ftl"], true, { generateBundlesSync });

  let values = l10n.formatValuesSync([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], null);

  l10n.addResourceIds(["/toolkit/menu.ftl"]);

  values = l10n.formatValuesSync([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  values = l10n.formatValuesSync(["key1", {id: "key2"}]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  values = l10n.formatValuesSync([{id: "key1"}, "key2"]);

  strictEqual(values[0], "Value1");
  strictEqual(values[1], "Value2");

  l10n.removeResourceIds(["/browser/menu.ftl"]);

  values = l10n.formatValuesSync([{id: "key1"}, {id: "key2"}]);

  strictEqual(values[0], null);
  strictEqual(values[1], "Value2");

  L10nRegistry.sources.clear();
  L10nRegistry.loadSync = originalLoadSync;
  Services.locale.requestedLocales = originalRequested;
});

add_task(function test_calling_sync_methods_in_async_mode_fails() {
  const l10n = new Localization(["/browser/menu.ftl"], false);

  Assert.throws(() => {
    l10n.formatValuesSync([{ id: "key1" }, { id: "key2" }]);
  }, /Can't use formatValuesSync when state is async./);

  Assert.throws(() => {
    l10n.formatValueSync("key1");
  }, /Can't use formatValueSync when state is async./);

  Assert.throws(() => {
    l10n.formatMessagesSync([{ id: "key1"}]);
  }, /Can't use formatMessagesSync when state is async./);
});
