/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// @ts-check

var perfMetadata = {
  owner: "Internationalization Team",
  name: "Intl.NumberFormat",
  description: "Test the speed of the Intl.NumberFormat implementation.",
  options: {
    default: {
      perfherder: true,
      perfherder_metrics: [
        {
          name: "Intl.NumberFormat constructor iterations",
          unit: "iterations",
        },
        { name: "Intl.NumberFormat constructor accumulatedTime", unit: "ms" },
        { name: "Intl.NumberFormat constructor perCallTime", unit: "ms" },

        {
          name: "Intl.NumberFormat.prototype.format iterations",
          unit: "iterations",
        },
        {
          name: "Intl.NumberFormat.prototype.format accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.NumberFormat.prototype.format perCallTime",
          unit: "ms",
        },

        {
          name: "Intl.NumberFormat.prototype.formatToParts iterations",
          unit: "iterations",
        },
        {
          name: "Intl.NumberFormat.prototype.formatToParts accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.NumberFormat.prototype.formatToParts perCallTime",
          unit: "ms",
        },
      ],
      verbose: true,
    },
  },
  tags: ["intl", "ecma402"],
};

add_task(function measure_numberformat() {
  const measureConstructor = measureIterations("Intl.NumberFormat constructor");
  const measureFormat = measureIterations("Intl.NumberFormat.prototype.format");
  const measureFormatToParts = measureIterations(
    "Intl.NumberFormat.prototype.formatToParts"
  );

  // Re-use the config between runs.

  const styles = ["decimal", "percent", "currency", "unit"];

  const numberStyles = [
    "arab",
    "arabext",
    "bali",
    "beng",
    "deva",
    "fullwide",
    "gujr",
    "guru",
    "hanidec",
    "khmr",
    "knda",
    "laoo",
    "latn",
    "limb",
    "mlym",
    "mong",
    "mymr",
    "orya",
    "tamldec",
    "telu",
    "thai",
    "tibt",
  ];

  const decimalOptions = {
    notation: ["scientific", "engineering", "compact"],
    useGroup: [true, false],
  };

  const currencyOptions = {
    currency: ["USD", "CAD", "EUR", "Yen", "MXN", "SAR", "INR", "CNY", "IDR"],
    currencyDisplay: ["symbol", "narrowSymbol", "code", "name"],
    currencySign: ["accounting", "standard"],
  };

  const unitOptions = {
    unit: [
      "acre",
      "bit",
      "byte",
      "celsius",
      "centimeter",
      "day",
      "degree",
      "fahrenheit",
      "fluid-ounce",
      "foot",
      "gallon",
      "gigabit",
      "gigabyte",
      "gram",
      "hectare",
      "hour",
      "inch",
      "kilobit",
      "kilobyte",
      "kilogram",
      "kilometer",
      "liter",
      "megabit",
      "megabyte",
      "meter",
      "mile",
      "mile-scandinavian",
      "milliliter",
      "millimeter",
      "millisecond",
      "minute",
      "month",
      "ounce",
      "percent",
      "petabyte",
      "pound",
      "second",
      "stone",
      "terabit",
      "terabyte",
      "week",
      "yard",
      "year",
      "meter-per-second",
      "kilometer-per-hour",
    ],
    unitDisplay: ["long", "short", "narrow"],
  };

  function choose(options) {
    return options[Math.floor(options.length * prng())];
  }

  function randomizeConfig(config, options) {
    for (let option in options) {
      config[option] = choose(options[option]);
    }
  }

  // Split each step of the benchmark into separate JS functions so that performance
  // profiles are easy to analyze.

  function benchmarkNumberFormatConstructor() {
    for (let i = 0; i < 1000; i++) {
      // Create a random configuration powered by a pseudo-random number generator. This
      // way the configurations will be the same between 2 different runs.
      const locale = pickRepresentativeLocale();
      const style = choose(styles);
      const nu = choose(numberStyles);
      let config = {
        style,
        nu,
      };
      if (style == "decimal") {
        randomizeConfig(config, decimalOptions);
      } else if (style == "currency") {
        randomizeConfig(config, currencyOptions);
      } else if (style == "unit") {
        randomizeConfig(config, unitOptions);
      }

      // Measure the constructor.
      measureConstructor.start();
      const formatter = Intl.NumberFormat(locale, config);
      // Also include one format operation to ensure the constructor is de-lazified.
      formatter.format(0);
      measureConstructor.stop();

      benchmarkFormatOperation(formatter);
      benchmarkFormatToPartsOperation(formatter);
    }
  }

  function benchmarkFormatOperation(formatter) {
    // Measure the format operation.
    for (let j = 0; j < 100; j++) {
      let num = -1e6 + prng() * 2e6;
      measureFormat.start();
      formatter.format(num);
      measureFormat.stop();
    }
  }

  function benchmarkFormatToPartsOperation(formatter) {
    // Measure the formatToParts operation.
    for (let j = 0; j < 100; j++) {
      let num = -1e6 + prng() * 2e6;
      measureFormatToParts.start();
      formatter.formatToParts(num);
      measureFormatToParts.stop();
    }
  }

  benchmarkNumberFormatConstructor();
  measureConstructor.reportMetrics();
  measureFormat.reportMetrics();
  measureFormatToParts.reportMetrics();

  ok(true);
});
