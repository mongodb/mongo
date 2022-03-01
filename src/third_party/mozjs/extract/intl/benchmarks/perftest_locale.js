/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// @ts-check

var perfMetadata = {
  owner: "Internationalization Team",
  name: "Intl.Locale",
  description: "Test the speed of the Intl.Locale implementation.",
  options: {
    default: {
      perfherder: true,
      perfherder_metrics: [
        {
          name: "Intl.Locale constructor iterations",
          unit: "iterations",
        },
        { name: "Intl.Locale constructor accumulatedTime", unit: "ms" },
        { name: "Intl.Locale constructor perCallTime", unit: "ms" },

        {
          name: "Intl.Locale.prototype accessors iterations",
          unit: "iterations",
        },
        {
          name: "Intl.Locale.prototype accessors accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.Locale.prototype accessors perCallTime",
          unit: "ms",
        },

        {
          name: "Intl.Locale.maximize operation iterations",
          unit: "iterations",
        },
        {
          name: "Intl.Locale.maximize operation accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.Locale.maximize operation perCallTime",
          unit: "ms",
        },
      ],
      verbose: true,
    },
  },
  tags: ["intl", "ecma402"],
};

const maximizeLocales = [
  "en-US",
  "en-GB",
  "es-AR",
  "it",
  "zh-Hans-CN",
  "de-AT",
  "pl",
  "fr-FR",
  "de-AT",
  "sr-Cyrl-SR",
  "nb-NO",
  "fr-FR",
  "mk",
  "uk",
  "und-PL",
  "und-Latn-AM",
  "ug-Cyrl",
  "sr-ME",
  "mn-Mong",
  "lif-Limb",
  "gan",
  "zh-Hant",
  "yue-Hans",
  "unr",
  "unr-Deva",
  "und-Thai-CN",
  "ug-Cyrl",
  "en-Latn-DE",
  "pl-FR",
  "de-CH",
  "tuq",
  "sr-ME",
  "ng",
  "klx",
  "kk-Arab",
  "en-Cyrl",
  "und-Cyrl-UK",
  "und-Arab",
  "und-Arab-FO",
];

add_task(function measure_locale() {
  const measureConstructor = measureIterations("Intl.Locale constructor");
  const measureAccessors = measureIterations("Intl.Locale.prototype accessors");
  const measureMaximize = measureIterations("Intl.Locale.maximize operation");

  // Split each step of the benchmark into separate JS functions so that performance
  // profiles are easy to analyze.

  function benchmarkDateTimeFormatConstructor() {
    for (let i = 0; i < 1000; i++) {
      // Create a random configuration powered by a pseudo-random number generator. This
      // way the configurations will be the same between 2 different runs.
      const localeString = pickRepresentativeLocale();

      // Measure the constructor.
      measureConstructor.start();
      const locale = new Intl.Locale(localeString);
      measureConstructor.stop();

      benchmarkAccessors(locale);
    }
  }

  const accessors = [
    "basename",
    "calendar",
    "caseFirst",
    "collation",
    "hourCycle",
    "numeric",
    "numberingSystem",
    "language",
    "script",
    "region",
  ];

  function benchmarkAccessors(locale) {
    for (let j = 0; j < 100; j++) {
      measureAccessors.start();
      for (let accessor in accessors) {
        locale[accessor];
      }
      measureAccessors.stop();
    }
  }

  function benchmarkMaximize() {
    let locales = [];
    for (let localeString of maximizeLocales) {
      locales.push(new Intl.Locale(localeString));
    }
    for (let j = 0; j < 10000; j++) {
      measureMaximize.start();
      for (let locale of locales) {
        locale.maximize();
      }
      measureMaximize.stop();
    }
  }

  benchmarkDateTimeFormatConstructor();
  benchmarkMaximize();
  measureConstructor.reportMetrics();
  measureAccessors.reportMetrics();
  measureMaximize.reportMetrics();

  ok(true);
});
