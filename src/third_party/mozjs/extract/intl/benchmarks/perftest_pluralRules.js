/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// @ts-check

var perfMetadata = {
  owner: "Internationalization Team",
  name: "Intl.PluralRules",
  description: "Test the speed of the Intl.PluralRules implementation.",
  options: {
    default: {
      perfherder: true,
      perfherder_metrics: [
        {
          name: "Intl.PluralRules constructor iterations",
          unit: "iterations",
        },
        { name: "Intl.PluralRules constructor accumulatedTime", unit: "ms" },
        { name: "Intl.PluralRules constructor perCallTime", unit: "ms" },

        {
          name: "Intl.PluralRules.prototype.select iterations",
          unit: "iterations",
        },
        {
          name: "Intl.PluralRules.prototype.select accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.PluralRules.prototype.select perCallTime",
          unit: "ms",
        },

        {
          name: "Intl.PluralRules pluralCategories iterations",
          unit: "iterations",
        },
        {
          name: "Intl.PluralRules pluralCategories accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.PluralRules pluralCategories perCallTime",
          unit: "ms",
        },
      ],
      verbose: true,
    },
  },
  tags: ["intl", "ecma402"],
};

add_task(function measure_pluralrules() {
  const measureConstructor = measureIterations("Intl.PluralRules constructor");
  const measureSelect = measureIterations("Intl.PluralRules.prototype.select");
  const measurePluralCategories = measureIterations(
    "Intl.PluralRules pluralCategories"
  );

  // Re-use the config between runs.

  const fieldOptions = {
    type: ["cardinal", "ordinal"],
  };

  const config = {};
  function randomizeConfig(name, chance) {
    const option = fieldOptions[name];
    if (prng() < chance) {
      config[name] = option[Math.floor(option.length * prng())];
    } else {
      delete config[name];
    }
  }

  // Split each step of the benchmark into separate JS functions so that performance
  // profiles are easy to analyze.

  function benchmarkPluralRulesConstructor() {
    for (let i = 0; i < 1000; i++) {
      // Create a random configuration powered by a pseudo-random number generator. This
      // way the configurations will be the same between 2 different runs.
      const locale = pickRepresentativeLocale();
      randomizeConfig("type", 0.5);

      // Measure the constructor.
      measureConstructor.start();
      const pr = new Intl.PluralRules(locale, config);
      measureConstructor.stop();

      benchmarkSelectOperation(pr);
      benchmarkPluralCategories(pr);
    }
  }

  function benchmarkSelectOperation(pr) {
    // Measure the select operation.
    for (let j = 0; j < 1000; j++) {
      // TODO: We may want to extend this to non-integer values in the future.
      const num = Math.floor(prng() * 10000);
      measureSelect.start();
      pr.select(num);
      measureSelect.stop();
    }
  }

  function benchmarkPluralCategories(pr) {
    measurePluralCategories.start();
    pr.resolvedOptions().pluralCategories;
    measurePluralCategories.stop();
  }

  benchmarkPluralRulesConstructor();
  measureConstructor.reportMetrics();
  measureSelect.reportMetrics();
  measurePluralCategories.reportMetrics();

  ok(true);
});
