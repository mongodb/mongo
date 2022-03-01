/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const { AppConstants } = ChromeUtils.import("resource://gre/modules/AppConstants.jsm");
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
// eslint-disable-next-line mozilla/use-services
const appinfo = Cc["@mozilla.org/xre/app-info;1"].getService(Ci.nsIXULRuntime);
const {XPCOMUtils} = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");
XPCOMUtils.defineLazyGlobalGetters(this, ["fetch"]);
ChromeUtils.defineModuleGetter(
  this,
  "NetUtil",
  "resource://gre/modules/NetUtil.jsm"
);

const isParentProcess = appinfo.processType === appinfo.PROCESS_TYPE_DEFAULT;
/**
 * L10nRegistry is a localization resource management system for Gecko.
 *
 * It manages the list of resource sources provided with the app and allows
 * for additional sources to be added and updated.
 *
 * It's primary purpose is to allow for building an iterator over FluentBundle objects
 * that will be utilized by a localization API.
 *
 * The generator creates all possible permutations of locales and sources to allow for
 * complete fallbacking.
 *
 * Example:
 *
 *   FileSource1:
 *     name: 'app'
 *     locales: ['en-US', 'de']
 *     resources: [
 *       '/browser/menu.ftl',
 *       '/platform/toolkit.ftl',
 *     ]
 *   FileSource2:
 *     name: 'platform'
 *     locales: ['en-US', 'de']
 *     resources: [
 *       '/platform/toolkit.ftl',
 *     ]
 *
 * If the user will request:
 *   L10nRegistry.generateBundles(['de', 'en-US'], [
 *     '/browser/menu.ftl',
 *     '/platform/toolkit.ftl'
 *   ]);
 *
 * the generator will return an async iterator over the following contexts:
 *
 *   {
 *     locale: 'de',
 *     resources: [
 *       ['app', '/browser/menu.ftl'],
 *       ['app', '/platform/toolkit.ftl'],
 *     ]
 *   },
 *   {
 *     locale: 'de',
 *     resources: [
 *       ['app', '/browser/menu.ftl'],
 *       ['platform', '/platform/toolkit.ftl'],
 *     ]
 *   },
 *   {
 *     locale: 'en-US',
 *     resources: [
 *       ['app', '/browser/menu.ftl'],
 *       ['app', '/platform/toolkit.ftl'],
 *     ]
 *   },
 *   {
 *     locale: 'en-US',
 *     resources: [
 *       ['app', '/browser/menu.ftl'],
 *       ['platform', '/platform/toolkit.ftl'],
 *     ]
 *   }
 *
 * This allows the localization API to consume the FluentBundle and lazily fallback
 * on the next in case of a missing string or error.
 *
 * If during the life-cycle of the app a new source is added, the generator can be called again
 * and will produce a new set of permutations placing the language pack provided resources
 * at the top.
 *
 * Notice: L10nRegistry is primarily an asynchronous API, but
 * it does provide a synchronous version of it's main method
 * for use  by the `Localization` class when in `sync` state.
 * This API should be only used in very specialized cases and
 * the uses should be reviewed by the toolkit owner/peer.
 */
class L10nRegistryService {
  constructor() {
    this.sources = new Map();

    if (isParentProcess) {
      const locales = Services.locale.packagedLocales;
      // Categories are sorted alphabetically, so we name our sources:
      //   - 0-toolkit
      //   - 5-browser
      //   - langpack-{locale}
      //
      // This should ensure that they're returned in the correct order.
      let fileSources = [];
      for (let {entry, value} of Services.catMan.enumerateCategory("l10n-registry")) {
        if (!this.hasSource(entry)) {
          fileSources.push(new FileSource(entry, locales, value));
        }
      }
      this.registerSources(fileSources);
    } else {
      this._setSourcesFromSharedData();
      Services.cpmm.sharedData.addEventListener("change", this);
    }
  }

  /**
   * Empty the sources to mimic shutdown for testing from xpcshell.
   */
  clearSources() {
    this.sources = new Map();
    Services.locale.availableLocales = this.getAvailableLocales();
  }

  handleEvent(event) {
    if (event.type === "change") {
      if (event.changedKeys.includes("L10nRegistry:Sources")) {
        this._setSourcesFromSharedData();
      }
    }
  }

  /**
   * Based on the list of requested languages and resource Ids,
   * this function returns an lazy iterator over message context permutations.
   *
   * Notice: Any changes to this method should be copied
   * to the `generateBundlesSync` equivalent below.
   *
   * @param {Array} requestedLangs
   * @param {Array} resourceIds
   * @returns {AsyncIterator<FluentBundle>}
   */
  async* generateBundles(requestedLangs, resourceIds) {
    const resourceIdsDedup = Array.from(new Set(resourceIds));
    const sourcesOrder = Array.from(this.sources.keys()).reverse();
    const pseudoStrategy = Services.prefs.getStringPref("intl.l10n.pseudo", "");
    for (const locale of requestedLangs) {
      for await (const dataSets of generateResourceSetsForLocale(locale, sourcesOrder, resourceIdsDedup)) {
        const bundle = new FluentBundle(locale, {
          ...MSG_CONTEXT_OPTIONS,
          pseudoStrategy,
        });
        for (const data of dataSets) {
          if (data === null) {
            return;
          }
          bundle.addResource(data);
        }
        yield bundle;
      }
    }
  }

  /**
   * This is a synchronous version of the `generateBundles`
   * method and should stay completely in sync with it at all
   * times except of the async/await changes.
   *
   * Notice: This method should be avoided at all costs
   * You can think of it similarly to a synchronous XMLHttpRequest.
   *
   * @param {Array} requestedLangs
   * @param {Array} resourceIds
   * @returns {Iterator<FluentBundle>}
   */
  * generateBundlesSync(requestedLangs, resourceIds) {
    const resourceIdsDedup = Array.from(new Set(resourceIds));
    const sourcesOrder = Array.from(this.sources.keys()).reverse();
    const pseudoStrategy = Services.prefs.getStringPref("intl.l10n.pseudo", "");
    for (const locale of requestedLangs) {
      for (const dataSets of generateResourceSetsForLocaleSync(locale, sourcesOrder, resourceIdsDedup)) {
        const bundle = new FluentBundle(locale, {
          ...MSG_CONTEXT_OPTIONS,
          pseudoStrategy
        });
        for (const data of dataSets) {
          if (data === null) {
            return;
          }
          bundle.addResource(data);
        }
        yield bundle;
      }
    }
  }

  /**
   * Check whether a source with the given known is already registered.
   *
   * @param {String} sourceName
   * @returns {boolean} whether or not a source by that name is known.
   */
  hasSource(sourceName) {
    return this.sources.has(sourceName);
  }

  /**
   * Adds new resource source(s) to the L10nRegistry.
   *
   * Notice: Each invocation of this method flushes any changes out to extant
   * content processes, which is expensive. Please coalesce multiple
   * registrations into a single sources array and then call this method once.
   *
   * @param {Array<FileSource>} sources
   */
  registerSources(sources) {
    for (const source of sources) {
      if (this.hasSource(source.name)) {
        throw new Error(`Source with name "${source.name}" already registered.`);
      }
      this.sources.set(source.name, source);
    }
    if (isParentProcess && sources.length > 0) {
      this._synchronizeSharedData();
      Services.locale.availableLocales = this.getAvailableLocales();
    }
  }

  /**
   * Updates existing sources in the L10nRegistry
   *
   * That will usually happen when a new version of a source becomes
   * available (for example, an updated version of a language pack).
   *
   * Notice: Each invocation of this method flushes any changes out to extant
   * content processes, which is expensive. Please coalesce multiple updates
   * into a single sources array and then call this method once.
   *
   * @param {Array<FileSource>} sources
   */
  updateSources(sources) {
    for (const source of sources) {
      if (!this.hasSource(source.name)) {
        throw new Error(`Source with name "${source.name}" is not registered.`);
      }
      this.sources.set(source.name, source);
    }
    if (isParentProcess && sources.length > 0) {
      this._synchronizeSharedData();
      Services.locale.availableLocales = this.getAvailableLocales();
    }
  }

  /**
   * Removes sources from the L10nRegistry.
   *
   * Notice: Each invocation of this method flushes any changes out to extant
   * content processes, which is expensive. Please coalesce multiple removals
   * into a single sourceNames array and then call this method once.
   *
   * @param {Array<String>} sourceNames
   */
  removeSources(sourceNames) {
    for (const sourceName of sourceNames) {
      this.sources.delete(sourceName);
    }
    if (isParentProcess && sourceNames.length > 0) {
      this._synchronizeSharedData();
      Services.locale.availableLocales = this.getAvailableLocales();
    }
  }

  _synchronizeSharedData() {
    const sources = new Map();
    for (const [name, source] of this.sources.entries()) {
      if (source.indexed) {
        continue;
      }
      sources.set(name, {
        locales: source.locales,
        prePath: source.prePath,
      });
    }
    let sharedData = Services.ppmm.sharedData;
    sharedData.set("L10nRegistry:Sources", sources);
    // We must explicitly flush or else flushing won't happen until the main
    // thread goes idle.
    sharedData.flush();
  }

  _setSourcesFromSharedData() {
    let sources = Services.cpmm.sharedData.get("L10nRegistry:Sources");
    if (!sources) {
      console.warn(`[l10nregistry] Failed to fetch sources from shared data.`);
      return;
    }
    let registerSourcesList = [];
    for (let [name, data] of sources.entries()) {
      if (!this.hasSource(name)) {
        const source = new FileSource(name, data.locales, data.prePath);
        registerSourcesList.push(source);
      }
    }
    this.registerSources(registerSourcesList);
    let removeSourcesList = [];
    for (let name of this.sources.keys()) {
      if (!sources.has(name)) {
        removeSourcesList.push(name);
      }
    }
    this.removeSources(removeSourcesList);
  }

  /**
   * Returns a list of locales for which at least one source
   * has resources.
   *
   * @returns {Array<String>}
   */
  getAvailableLocales() {
    const locales = new Set();

    for (const source of this.sources.values()) {
      for (const locale of source.locales) {
        locales.add(locale);
      }
    }
    return Array.from(locales);
  }
}

/**
 * This function generates an iterator over FluentBundles for a single locale
 * for a given list of resourceIds for all possible combinations of sources.
 *
 * This function is called recursively to generate all possible permutations
 * and uses the last, optional parameter, to pass the already resolved
 * sources order.
 *
 * Notice: Any changes to this method should be copied
 * to the `generateResourceSetsForLocaleSync` equivalent below.
 *
 * @param {String} locale
 * @param {Array} sourcesOrder
 * @param {Array} resourceIds
 * @param {Array} [resolvedOrder]
 * @returns {AsyncIterator<FluentBundle>}
 */
async function* generateResourceSetsForLocale(locale, sourcesOrder, resourceIds, resolvedOrder = []) {
  const resolvedLength = resolvedOrder.length;
  const resourcesLength = resourceIds.length;

  // Inside that loop we have a list of resources and the sources for them, like this:
  //   ['test.ftl', 'menu.ftl', 'foo.ftl']
  //   ['app', 'platform', 'app']
  for (const sourceName of sourcesOrder) {
    const order = resolvedOrder.concat(sourceName);

    // We want to bail out early if we know that any of
    // the (res)x(source) combinations in the permutation
    // are unavailable.
    // The combination may have been `undefined` when we
    // stepped into this branch, and now is resolved to
    // `false`.
    //
    // If the combination resolved to `false` is the last
    // in the resolvedOrder, we want to continue in this
    // loop, but if it's somewhere in the middle, we can
    // safely bail from the whole branch.
    for (let [idx, sourceName] of order.entries()) {
      const source = L10nRegistry.sources.get(sourceName);
      if (!source || source.hasFile(locale, resourceIds[idx]) === false) {
        if (idx === order.length - 1) {
          continue;
        } else {
          return;
        }
      }
    }

    // If the number of resolved sources equals the number of resources,
    // create the right context and return it if it loads.
    if (resolvedLength + 1 === resourcesLength) {
      let dataSet = await generateResourceSet(locale, order, resourceIds);
      // Here we check again to see if the newly resolved
      // resources returned `false` on any position.
      if (!dataSet.includes(false)) {
        yield dataSet;
      }
    } else if (resolvedLength < resourcesLength) {
      // otherwise recursively load another generator that walks over the
      // partially resolved list of sources.
      yield * generateResourceSetsForLocale(locale, sourcesOrder, resourceIds, order);
    }
  }
}

/**
 * This is a synchronous version of the `generateResourceSetsForLocale`
 * method and should stay completely in sync with it at all
 * times except of the async/await changes.
 *
 * @param {String} locale
 * @param {Array} sourcesOrder
 * @param {Array} resourceIds
 * @param {Array} [resolvedOrder]
 * @returns {Iterator<FluentBundle>}
 */
function* generateResourceSetsForLocaleSync(locale, sourcesOrder, resourceIds, resolvedOrder = []) {
  const resolvedLength = resolvedOrder.length;
  const resourcesLength = resourceIds.length;

  // Inside that loop we have a list of resources and the sources for them, like this:
  //   ['test.ftl', 'menu.ftl', 'foo.ftl']
  //   ['app', 'platform', 'app']
  for (const sourceName of sourcesOrder) {
    const order = resolvedOrder.concat(sourceName);

    // We want to bail out early if we know that any of
    // the (res)x(source) combinations in the permutation
    // are unavailable.
    // The combination may have been `undefined` when we
    // stepped into this branch, and now is resolved to
    // `false`.
    //
    // If the combination resolved to `false` is the last
    // in the resolvedOrder, we want to continue in this
    // loop, but if it's somewhere in the middle, we can
    // safely bail from the whole branch.
    for (let [idx, sourceName] of order.entries()) {
      const source = L10nRegistry.sources.get(sourceName);
      if (!source || source.hasFile(locale, resourceIds[idx]) === false) {
        if (idx === order.length - 1) {
          continue;
        } else {
          return;
        }
      }
    }

    // If the number of resolved sources equals the number of resources,
    // create the right context and return it if it loads.
    if (resolvedLength + 1 === resourcesLength) {
      let dataSet = generateResourceSetSync(locale, order, resourceIds);
      // Here we check again to see if the newly resolved
      // resources returned `false` on any position.
      if (!dataSet.includes(false)) {
        yield dataSet;
      }
    } else if (resolvedLength < resourcesLength) {
      // otherwise recursively load another generator that walks over the
      // partially resolved list of sources.
      yield * generateResourceSetsForLocaleSync(locale, sourcesOrder, resourceIds, order);
    }
  }
}

const MSG_CONTEXT_OPTIONS = {
  // Temporarily disable bidi isolation due to Microsoft not supporting FSI/PDI.
  // See bug 1439018 for details.
  useIsolating: Services.prefs.getBoolPref("intl.l10n.enable-bidi-marks", false),
};

/**
 * Generates a single FluentBundle by loading all resources
 * from the listed sources for a given locale.
 *
 * The function casts all error cases into a Promise that resolves with
 * value `null`.
 * This allows the caller to be an async generator without using
 * try/catch clauses.
 *
 * Notice: Any changes to this method should be copied
 * to the `generateResourceSetSync` equivalent below.
 *
 * @param {String} locale
 * @param {Array} sourcesOrder
 * @param {Array} resourceIds
 * @returns {Promise<FluentBundle>}
 */
function generateResourceSet(locale, sourcesOrder, resourceIds) {
  return Promise.all(resourceIds.map((resourceId, i) => {
    const source = L10nRegistry.sources.get(sourcesOrder[i]);
    if (!source) {
      return false;
    }
    return source.fetchFile(locale, resourceId);
  }));
}

/**
 * This is a synchronous version of the `generateResourceSet`
 * method and should stay completely in sync with it at all
 * times except of the async/await changes.
 *
 * @param {String} locale
 * @param {Array} sourcesOrder
 * @param {Array} resourceIds
 * @returns {FluentBundle}
 */
function generateResourceSetSync(locale, sourcesOrder, resourceIds) {
  return resourceIds.map((resourceId, i) => {
    const source = L10nRegistry.sources.get(sourcesOrder[i]);
    if (!source) {
      return false;
    }
    return source.fetchFile(locale, resourceId, {sync: true});
  });
}

/**
 * This is a basic Source for L10nRegistry.
 * It registers its own locales and a pre-path, and when asked for a file
 * it attempts to download and cache it.
 *
 * The Source caches the downloaded files so any consecutive loads will
 * come from the cache.
 **/
class FileSource {
  /**
   * @param {string}         name
   * @param {Array<string>}  locales
   * @param {string}         prePath
   *
   * @returns {FileSource}
   */
  constructor(name, locales, prePath) {
    this.name = name;
    this.locales = locales;
    this.prePath = prePath;
    this.indexed = false;

    // The cache object stores information about the resources available
    // in the Source.
    //
    // It can take one of three states:
    //   * true - the resource is available but not fetched yet
    //   * false - the resource is not available
    //   * Promise - the resource has been fetched
    //
    // If the cache has no entry for a given path, that means that there
    // is no information available about whether the resource is available.
    //
    // If the `indexed` property is set to `true` it will be treated as the
    // resource not being available. Otherwise, the resource may be
    // available and we do not have any information about it yet.
    this.cache = {};
  }

  getPath(locale, path) {
    // This is a special case for the only not BCP47-conformant locale
    // code we have resources for.
    if (locale === "ja-JP-macos") {
      locale = "ja-JP-mac";
    }
    return (this.prePath + path).replace(/\{locale\}/g, locale);
  }

  hasFile(locale, path) {
    if (!this.locales.includes(locale)) {
      return false;
    }

    const fullPath = this.getPath(locale, path);
    if (!this.cache.hasOwnProperty(fullPath)) {
      return this.indexed ? false : undefined;
    }
    if (this.cache[fullPath] === false) {
      return false;
    }
    if (this.cache[fullPath].then) {
      return undefined;
    }
    return true;
  }

  fetchFile(locale, path, options = {sync: false}) {
    if (!this.locales.includes(locale)) {
      return false;
    }

    const fullPath = this.getPath(locale, path);

    if (this.cache.hasOwnProperty(fullPath)) {
      if (this.cache[fullPath] === false) {
        return false;
      }
      // `true` means that the file is indexed, but hasn't
      // been fetched yet.
      if (this.cache[fullPath] !== true) {
        if (this.cache[fullPath] instanceof Promise && options.sync) {
          console.warn(`[l10nregistry] Attempting to synchronously load file
            ${fullPath} while it's being loaded asynchronously.`);
        } else {
          return this.cache[fullPath];
        }
      }
    } else if (this.indexed) {
      return false;
    }
    if (options.sync) {
      let data = L10nRegistry.loadSync(fullPath);

      if (data === false) {
        this.cache[fullPath] = false;
      } else {
        this.cache[fullPath] = new FluentResource(data);
      }

      return this.cache[fullPath];
    }

    // async
    return this.cache[fullPath] = L10nRegistry.load(fullPath).then(
      data => {
        return this.cache[fullPath] = new FluentResource(data);
      },
      err => {
        this.cache[fullPath] = false;
        return false;
      }
    );
  }
}

/**
 * This is an extension of the FileSource which should be used
 * for sources that can provide the list of files available in the source.
 *
 * This allows for a faster lookup in cases where the source does not
 * contain most of the files that the app will request for (e.g. an addon).
 **/
class IndexedFileSource extends FileSource {
  /**
   * @param {string}         name
   * @param {Array<string>}  locales
   * @param {string}         prePath
   * @param {Array<string>}  paths
   *
   * @returns {IndexedFileSource}
   */
  constructor(name, locales, prePath, paths) {
    super(name, locales, prePath);
    this.indexed = true;
    for (const path of paths) {
      this.cache[path] = true;
    }
  }
}

this.L10nRegistry = new L10nRegistryService();

/**
 * The low level wrapper around Fetch API. It unifies the error scenarios to
 * always produce a promise rejection.
 *
 * We keep it as a method to make it easier to override for testing purposes.
 *
 * @param {string} url
 *
 * @returns {Promise<string>}
 */
L10nRegistry.load = function(url) {
  return fetch(url).then(response => {
    if (!response.ok) {
      return Promise.reject(response.statusText);
    }
    return response.text();
  });
};

/**
 * This is a synchronous version of the `load`
 * function and should stay completely in sync with it at all
 * times except of the async/await changes.
 *
 * Notice: Any changes to this method should be copied
 * to the `generateResourceSetSync` equivalent below.
 *
 * @param {string} url
 *
 * @returns {string}
 */
L10nRegistry.loadSync = function(uri) {
  try {
    let url = Services.io.newURI(uri);
    let data = Cu.readUTF8URI(url);
    return data;
  } catch (e) {
    if (
      e.result == Cr.NS_ERROR_INVALID_ARG ||
      e.result == Cr.NS_ERROR_NOT_INITIALIZED
    ) {
      try {
        // The preloader doesn't support this url or isn't initialized
        // (xpcshell test). Try a synchronous channel load.
        let stream = NetUtil.newChannel({
          uri,
          loadUsingSystemPrincipal: true,
        }).open();

        return NetUtil.readInputStreamToString(stream, stream.available(), {
          charset: "UTF-8",
        });
      } catch (e) {
        if (e.result != Cr.NS_ERROR_FILE_NOT_FOUND) {
          Cu.reportError(e);
        }
      }
    } else if (e.result != Cr.NS_ERROR_FILE_NOT_FOUND) {
      Cu.reportError(e);
    }
  }

  return false;
};

this.FileSource = FileSource;
this.IndexedFileSource = IndexedFileSource;

var EXPORTED_SYMBOLS = ["L10nRegistry", "FileSource", "IndexedFileSource"];
