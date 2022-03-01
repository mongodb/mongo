/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Create an interface to measure iterations for a micro benchmark. These iterations
 * will then be reported to the perftest runner.
 *
 * @param {string} metricName
 */
function measureIterations(metricName) {
  let accumulatedTime = 0;
  let iterations = 0;
  let now = 0;
  return {
    /**
     * Start a measurement.
     */
    start() {
      now = Cu.now();
    },
    /**
     * Stop a measurement, and record the elapsed time.
     */
    stop() {
      accumulatedTime += Cu.now() - now;
      iterations++;
    },
    /**
     * Report the metrics to perftest after finishing the microbenchmark.
     */
    reportMetrics() {
      const metrics = {};
      metrics[metricName + " iterations"] = iterations;
      metrics[metricName + " accumulatedTime"] = accumulatedTime;
      metrics[metricName + " perCallTime"] = accumulatedTime / iterations;

      info("perfMetrics", metrics);
    },
  };
}

let _seed = 123456;
/**
 * A cheap and simple pseudo-random number generator that avoids adding new dependencies.
 * This function ensures tests are repeatable, but can be fed random configurations.
 *
 * https://en.wikipedia.org/wiki/Linear_congruential_generator
 *
 * It has the following distribution for the first 100,000 runs:
 *
 *    0.0 - 0.1: 9948
 *    0.1 - 0.2: 10037
 *    0.2 - 0.3: 10049
 *    0.3 - 0.4: 10041
 *    0.4 - 0.5: 10036
 *    0.5 - 0.6: 10085
 *    0.6 - 0.7: 9987
 *    0.7 - 0.8: 9872
 *    0.8 - 0.9: 10007
 *    0.9 - 1.0: 9938
 *
 * @returns {number} float values ranged 0-1
 */
function prng() {
  _seed = Math.imul(_seed, 22695477) + 1;
  return (_seed >> 1) / 0x7fffffff + 0.5;
}

/**
 * The distribution of locales. The number represents the ratio of total users in that
 * locale. The numbers should add up to ~1.0.
 *
 * https://sql.telemetry.mozilla.org/dashboard/firefox-localization
 */
const localeDistribution = {
  "en-US": 0.373,
  de: 0.129,
  fr: 0.084,
  "zh-CN": 0.053,
  ru: 0.048,
  "es-ES": 0.047,
  pl: 0.041,
  "pt-BR": 0.034,
  it: 0.028,
  "en-GB": 0.027,
  ja: 0.019,
  "es-MX": 0.014,
  nl: 0.01,
  cs: 0.009,
  hu: 0.008,
  id: 0.006,
  "en-CA": 0.006,
  "es-AR": 0.006,
  tr: 0.005,
  el: 0.005,
  "zh-TW": 0.005,
  fi: 0.005,
  "sv-SE": 0.004,
  "pt-PT": 0.004,
  sk: 0.003,
  ar: 0.003,
  vi: 0.003,
  "es-CL": 0.002,
  th: 0.002,
  da: 0.002,
  bg: 0.002,
  ro: 0.002,
  "nb-NO": 0.002,
  ko: 0.002,
};

/**
 * Go through the top Firefox locales, and pick one at random that is representative
 * of the Firefox population as of 2021-06-03. It uses a pseudo-random number generator
 * to make the results repeatable.
 *
 * @returns {string} locale
 */
function pickRepresentativeLocale() {
  const n = prng();
  let ratio = 1;
  for (const [locale, representation] of Object.entries(localeDistribution)) {
    ratio -= representation;
    if (n > ratio) {
      return locale;
    }
  }
  // In case we fall through the "for" loop, return the most common locale.
  return "en-US";
}
