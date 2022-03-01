/* vim: set ts=2 et sw=2 tw=80 filetype=javascript: */

/* Copyright 2017 Mozilla Foundation and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/* fluent-dom@fa25466f (October 12, 2018) */

/* eslint no-console: ["error", { allow: ["warn", "error"] }] */
/* global console */

const { L10nRegistry } = ChromeUtils.import("resource://gre/modules/L10nRegistry.jsm");
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { AppConstants } = ChromeUtils.import("resource://gre/modules/AppConstants.jsm");

/*
 * Base CachedIterable class.
 */
class CachedIterable extends Array {
  /**
   * Create a `CachedIterable` instance from an iterable or, if another
   * instance of `CachedIterable` is passed, return it without any
   * modifications.
   *
   * @param {Iterable} iterable
   * @returns {CachedIterable}
   */
  static from(iterable) {
    if (iterable instanceof this) {
      return iterable;
    }

    return new this(iterable);
  }
}

/*
 * CachedAsyncIterable caches the elements yielded by an async iterable.
 *
 * It can be used to iterate over an iterable many times without depleting the
 * iterable.
 */
class CachedAsyncIterable extends CachedIterable {
  /**
   * Create an `CachedAsyncIterable` instance.
   *
   * @param {Iterable} iterable
   * @returns {CachedAsyncIterable}
   */
  constructor(iterable) {
    super();

    if (Symbol.asyncIterator in Object(iterable)) {
      this.iterator = iterable[Symbol.asyncIterator]();
    } else if (Symbol.iterator in Object(iterable)) {
      this.iterator = iterable[Symbol.iterator]();
    } else {
      throw new TypeError("Argument must implement the iteration protocol.");
    }
  }

  /**
   * Asynchronous iterator caching the yielded elements.
   *
   * Elements yielded by the original iterable will be cached and available
   * synchronously. Returns an async generator object implementing the
   * iterator protocol over the elements of the original (async or sync)
   * iterable.
   */
  [Symbol.asyncIterator]() {
    const cached = this;
    let cur = 0;

    return {
      async next() {
        if (cached.length <= cur) {
          cached.push(cached.iterator.next());
        }
        return cached[cur++];
      },
    };
  }

  /**
   * This method allows user to consume the next element from the iterator
   * into the cache.
   *
   * @param {number} count - number of elements to consume
   */
  async touchNext(count = 1) {
    let idx = 0;
    while (idx++ < count) {
      const last = this[this.length - 1];
      if (last && (await last).done) {
        break;
      }
      this.push(this.iterator.next());
    }
    // Return the last cached {value, done} object to allow the calling
    // code to decide if it needs to call touchNext again.
    return this[this.length - 1];
  }
}

/*
 * CachedSyncIterable caches the elements yielded by an iterable.
 *
 * It can be used to iterate over an iterable many times without depleting the
 * iterable.
 */
class CachedSyncIterable extends CachedIterable {
    /**
     * Create an `CachedSyncIterable` instance.
     *
     * @param {Iterable} iterable
     * @returns {CachedSyncIterable}
     */
    constructor(iterable) {
        super();

        if (Symbol.iterator in Object(iterable)) {
            this.iterator = iterable[Symbol.iterator]();
        } else {
            throw new TypeError("Argument must implement the iteration protocol.");
        }
    }

    [Symbol.iterator]() {
        const cached = this;
        let cur = 0;

        return {
            next() {
                if (cached.length <= cur) {
                    cached.push(cached.iterator.next());
                }
                return cached[cur++];
            },
        };
    }

    /**
     * This method allows user to consume the next element from the iterator
     * into the cache.
     *
     * @param {number} count - number of elements to consume
     */
    touchNext(count = 1) {
        let idx = 0;
        while (idx++ < count) {
            const last = this[this.length - 1];
            if (last && last.done) {
                break;
            }
            this.push(this.iterator.next());
        }
        // Return the last cached {value, done} object to allow the calling
        // code to decide if it needs to call touchNext again.
        return this[this.length - 1];
    }
}

/**
 * The default localization strategy for Gecko. It comabines locales
 * available in L10nRegistry, with locales requested by the user to
 * generate the iterator over FluentBundles.
 *
 * In the future, we may want to allow certain modules to override this
 * with a different negotitation strategy to allow for the module to
 * be localized into a different language - for example DevTools.
 */
function defaultGenerateBundles(resourceIds) {
  const appLocales = Services.locale.appLocalesAsBCP47;
  return L10nRegistry.generateBundles(appLocales, resourceIds);
}

function defaultGenerateBundlesSync(resourceIds) {
  const appLocales = Services.locale.appLocalesAsBCP47;
  return L10nRegistry.generateBundlesSync(appLocales, resourceIds);
}

function maybeReportErrorToGecko(error) {
  if (AppConstants.NIGHTLY_BUILD || Cu.isInAutomation) {
    if (Cu.isInAutomation) {
      // We throw a string, rather than Error
      // to allow the C++ Promise handler
      // to clone it
      throw error;
    }
    console.warn(error);
  }
}

/**
 * The `Localization` class is a central high-level API for vanilla
 * JavaScript use of Fluent.
 * It combines language negotiation, FluentBundle and I/O to
 * provide a scriptable API to format translations.
 */
const Localization = {
  cached(iterable, isSync) {
    if (isSync) {
      return CachedSyncIterable.from(iterable);
    } else {
      return CachedAsyncIterable.from(iterable);
    }
  },

  /**
   * Format translations and handle fallback if needed.
   *
   * Format translations for `keys` from `FluentBundle` instances on this
   * Localization. In case of errors, fetch the next context in the
   * fallback chain.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @param   {Function}              method  - Formatting function.
   * @returns {Promise<Array<string?|Object?>>}
   * @private
   */
  async formatWithFallback(resourceIds, bundles, keys, method) {
    if (!bundles) {
      throw new Error("Attempt to format on an uninitialized instance.");
    }

    const translations = new Array(keys.length).fill(null);
    let hasAtLeastOneBundle = false;

    for await (const bundle of bundles) {
      hasAtLeastOneBundle = true;
      const missingIds = keysFromBundle(method, bundle, keys, translations);

      if (missingIds.size === 0) {
        break;
      }

      const locale = bundle.locales[0];
      const ids = Array.from(missingIds).join(", ");
      maybeReportErrorToGecko(`[fluent] Missing translations in ${locale}: ${ids}.`);
    }

    if (!hasAtLeastOneBundle) {
      maybeReportErrorToGecko(`[fluent] Request for keys failed because no resource bundles got generated.\n keys: ${JSON.stringify(keys)}.\n resourceIds: ${JSON.stringify(resourceIds)}.`);
    }

    return translations;
  },

  /**
   * Format translations and handle fallback if needed.
   *
   * Format translations for `keys` from `FluentBundle` instances on this
   * Localization. In case of errors, fetch the next context in the
   * fallback chain.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @param   {Function}              method  - Formatting function.
   * @returns {Array<string|Object>}
   * @private
   */
  formatWithFallbackSync(resourceIds, bundles, keys, method) {
    if (!bundles) {
      throw new Error("Attempt to format on an uninitialized instance.");
    }

    const translations = new Array(keys.length).fill(null);
    let hasAtLeastOneBundle = false;

    for (const bundle of bundles) {
      hasAtLeastOneBundle = true;
      const missingIds = keysFromBundle(method, bundle, keys, translations);

      if (missingIds.size === 0) {
        break;
      }

      const locale = bundle.locales[0];
      const ids = Array.from(missingIds).join(", ");
      maybeReportErrorToGecko(`[fluent] Missing translations in ${locale}: ${ids}.`);
    }

    if (!hasAtLeastOneBundle) {
      maybeReportErrorToGecko(`[fluent] Request for keys failed because no resource bundles got generated.\n keys: ${JSON.stringify(keys)}.\n resourceIds: ${JSON.stringify(resourceIds)}.`);
    }

    return translations;
  },


  /**
   * Format translations into {value, attributes} objects.
   *
   * The fallback logic is the same as in `formatValues` but it returns {value,
   * attributes} objects which are suitable for the translation of DOM
   * elements.
   *
   *     docL10n.formatMessages([
   *       {id: 'hello', args: { who: 'Mary' }},
   *       {id: 'welcome'}
   *     ]).then(console.log);
   *
   *     // [
   *     //   { value: 'Hello, Mary!', attributes: null },
   *     //   {
   *     //     value: 'Welcome!',
   *     //     attributes: [ { name: "title", value: 'Hello' } ]
   *     //   }
   *     // ]
   *
   * Returns a Promise resolving to an array of the translation messages.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @returns {Promise<Array<{value: string, attributes: Object}?>>}
   * @private
   */
  formatMessages(resourceIds, bundles, keys) {
    return this.formatWithFallback(resourceIds, bundles, keys, messageFromBundle);
  },

  /**
   * Sync version of `formatMessages`.
   *
   * Returns an array of the translation messages.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @returns {Array<{value: string, attributes: Object}?>}
   * @private
   */
  formatMessagesSync(resourceIds, bundles, keys) {
    return this.formatWithFallbackSync(resourceIds, bundles, keys, messageFromBundle);
  },

  /**
   * Retrieve translations corresponding to the passed keys.
   *
   * A generalized version of `Localization.formatValue`. Keys must
   * be `{id, args}` objects.
   *
   *     docL10n.formatValues([
   *       {id: 'hello', args: { who: 'Mary' }},
   *       {id: 'hello', args: { who: 'John' }},
   *       {id: 'welcome'}
   *     ]).then(console.log);
   *
   *     // ['Hello, Mary!', 'Hello, John!', 'Welcome!']
   *
   * Returns a Promise resolving to an array of the translation strings.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @returns {Promise<Array<string?>>}
   */
  formatValues(resourceIds, bundles, keys) {
    return this.formatWithFallback(resourceIds, bundles, keys, valueFromBundle);
  },

  /**
   * Sync version of `formatValues`.
   *
   * Returns an array of the translation strings.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {Array<string|Object>}  keys    - Translation keys to format.
   * @returns {Array<string?>}
   * @private
   */
  formatValuesSync(resourceIds, bundles, keys) {
    return this.formatWithFallbackSync(resourceIds, bundles, keys, valueFromBundle);
  },

  /**
   * Retrieve the translation corresponding to the `id` identifier.
   *
   * If passed, `args` is a simple hash object with a list of variables that
   * will be interpolated in the value of the translation.
   *
   *     docL10n.formatValue(
   *       'hello', { who: 'world' }
   *     ).then(console.log);
   *
   *     // 'Hello, world!'
   *
   * Returns a Promise resolving to a translation string.
   *
   * Use this sparingly for one-off messages which don't need to be
   * retranslated when the user changes their language preferences, e.g. in
   * notifications.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {string}                 id     - Identifier of the translation to format
   * @param   {Object}                 [args] - Optional external arguments
   * @returns {Promise<string?>}
   */
  async formatValue(resourceIds, bundles, id, args) {
    const [val] = await this.formatValues(resourceIds, bundles, [{id, args}]);
    return val;
  },

  /**
   * Sync version of `formatValue`.
   *
   * Returns a translation string.
   *
   * @param   {Array<String>}     resourceIds - List of resource ids used by this
   *                                            localization.
   * @param   {Iter<FluentBundle>}    bundles - Iterator over bundles.
   * @param   {string}                 id     - Identifier of the translation to format
   * @param   {Object}                 [args] - Optional external arguments
   * @returns {string?}
   * @private
   */
  formatValueSync(resourceIds, bundles, id, args) {
    const [val] = this.formatValuesSync(resourceIds, bundles, [{id, args}]);
    return val;
  },

  /**
   * This method should be called when there's a reason to believe
   * that language negotiation or available resources changed.
   *
   * @param {Array<String>}    resourceIds - List of resource ids used by this
   *                                         localization.
   * @param {bool}                  isSync - Whether the instance should be
   *                                         synchronous.
   * @param {bool}                   eager - whether the I/O for new context should begin eagerly
   * @param {Function}     generateBundles - Custom FluentBundle asynchronous generator.
   * @param {Function} generateBundlesSync - Custom FluentBundle generator.
   * @returns {Iter<FluentBundle>}
   */
  generateBundles(resourceIds, isSync, eager = false, generateBundles = defaultGenerateBundles, generateBundlesSync = defaultGenerateBundlesSync) {
    // Store for error reporting from `formatWithFallback`.
    let generateMessages = isSync ? generateBundlesSync : generateBundles;
    let bundles = this.cached(generateMessages(resourceIds), isSync);
    if (eager) {
      // If the first app locale is the same as last fallback
      // it means that we have all resources in this locale, and
      // we want to eagerly fetch just that one.
      // Otherwise, we're in a scenario where the first locale may
      // be partial and we want to eagerly fetch a fallback as well.
      const appLocale = Services.locale.appLocaleAsBCP47;
      const lastFallback = Services.locale.lastFallbackLocale;
      const prefetchCount = appLocale === lastFallback ? 1 : 2;
      bundles.touchNext(prefetchCount);
    }
    return bundles;
  },
}

/**
 * Format the value of a message into a string or `null`.
 *
 * This function is passed as a method to `keysFromBundle` and resolve
 * a value of a single L10n Entity using provided `FluentBundle`.

 * If the message doesn't have a value, return `null`.
 *
 * @param   {FluentBundle} bundle
 * @param   {Array<Error>} errors
 * @param   {Object} message
 * @param   {Object} args
 * @returns {string?}
 * @private
 */
function valueFromBundle(bundle, errors, message, args) {
  if (message.value) {
    return bundle.formatPattern(message.value, args, errors);
  }

  return null;
}

/**
 * Format all public values of a message into a {value, attributes} object.
 *
 * This function is passed as a method to `keysFromBundle` and resolve
 * a single L10n Entity using provided `FluentBundle`.
 *
 * The function will return an object with a value and attributes of the
 * entity.
 *
 * @param   {FluentBundle} bundle
 * @param   {Array<Error>}   errors
 * @param   {Object} message
 * @param   {Object} args
 * @returns {Object}
 * @private
 */
function messageFromBundle(bundle, errors, message, args) {
  const formatted = {
    value: null,
    attributes: null,
  };

  if (message.value) {
    formatted.value = bundle.formatPattern(message.value, args, errors);
  }

  let attrNames = Object.keys(message.attributes);
  if (attrNames.length > 0) {
    formatted.attributes = new Array(attrNames.length);
    for (let [i, name] of attrNames.entries()) {
      let value = bundle.formatPattern(message.attributes[name], args, errors);
      formatted.attributes[i] = {name, value};
    }
  }

  return formatted;
}

/**
 * This function is an inner function for `Localization.formatWithFallback`.
 *
 * It takes a `FluentBundle`, list of l10n-ids and a method to be used for
 * key resolution (either `valueFromBundle` or `messageFromBundle`) and
 * optionally a value returned from `keysFromBundle` executed against
 * another `FluentBundle`.
 *
 * The idea here is that if the previous `FluentBundle` did not resolve
 * all keys, we're calling this function with the next context to resolve
 * the remaining ones.
 *
 * In the function, we loop over `keys` and check if we have the `prev`
 * passed and if it has an error entry for the position we're in.
 *
 * If it doesn't, it means that we have a good translation for this key and
 * we return it. If it does, we'll try to resolve the key using the passed
 * `FluentBundle`.
 *
 * In the end, we fill the translations array, and return the Set with
 * missing ids.
 *
 * See `Localization.formatWithFallback` for more info on how this is used.
 *
 * @param {Function}       method
 * @param {FluentBundle}   bundle
 * @param {Array<string|Object>} keys
 * @param {{Array<{value: string, attributes: Object}>}} translations
 *
 * @returns {Set<string>}
 * @private
 */
function keysFromBundle(method, bundle, keys, translations) {
  const messageErrors = [];
  const missingIds = new Set();

  keys.forEach((key, i) => {
    let id;
    let args = undefined;
    if (typeof key == "object" && "id" in key) {
      id = String(key.id);
      args = key.args;
    } else {
      id = String(key);
    }

    if (translations[i] !== null) {
      return;
    }

    let message = bundle.getMessage(id);
    if (message) {
      messageErrors.length = 0;
      translations[i] = method(bundle, messageErrors, message, args);
      if (messageErrors.length > 0) {
        const locale = bundle.locales[0];
        const errors = messageErrors.join(", ");
        maybeReportErrorToGecko(`[fluent][resolver] errors in ${locale}/${id}: ${errors}.`);
      }
    } else {
      missingIds.add(id);
    }
  });

  return missingIds;
}

this.Localization = Localization;
var EXPORTED_SYMBOLS = ["Localization"];
