/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// @ts-check

var perfMetadata = {
  owner: "Internationalization Team",
  name: "Intl.DateTimeFormat",
  description: "Test the speed of the Intl.DateTimeFormat implementation.",
  options: {
    default: {
      perfherder: true,
      perfherder_metrics: [
        {
          name: "Intl.DateTimeFormat constructor iterations",
          unit: "iterations",
        },
        { name: "Intl.DateTimeFormat constructor accumulatedTime", unit: "ms" },
        { name: "Intl.DateTimeFormat constructor perCallTime", unit: "ms" },

        {
          name: "Intl.DateTimeFormat.prototype.format iterations",
          unit: "iterations",
        },
        {
          name: "Intl.DateTimeFormat.prototype.format accumulatedTime",
          unit: "ms",
        },
        {
          name: "Intl.DateTimeFormat.prototype.format perCallTime",
          unit: "ms",
        },
      ],
      verbose: true,
    },
  },
  tags: ["intl", "ecma402"],
};

add_task(function measure_date() {
  const measureConstructor = measureIterations(
    "Intl.DateTimeFormat constructor"
  );
  const measureFormat = measureIterations(
    "Intl.DateTimeFormat.prototype.format"
  );

  // Re-use the config between runs.

  const fieldOptions = {
    weekday: ["narrow", "short", "long"],
    era: ["narrow", "short", "long"],
    year: ["2-digit", "numeric"],
    month: ["2-digit", "numeric", "narrow", "short", "long"],
    day: ["2-digit", "numeric"],
    hour: ["2-digit", "numeric"],
    minute: ["2-digit", "numeric"],
    second: ["2-digit", "numeric"],
    timeZoneName: ["short", "long"],
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

  let date = new Date(Date.UTC(2020, 11, 20, 3, 23, 16, 738));

  // Split each step of the benchmark into separate JS functions so that performance
  // profiles are easy to analyze.

  function benchmarkDateTimeFormatConstructor() {
    for (let i = 0; i < 1000; i++) {
      // Create a random configuration powered by a pseudo-random number generator. This
      // way the configurations will be the same between 2 different runs.
      const locale = pickRepresentativeLocale();
      randomizeConfig("year", 0.5);
      randomizeConfig("month", 0.5);
      randomizeConfig("day", 0.5);
      randomizeConfig("hour", 0.5);
      randomizeConfig("minute", 0.5);
      // Set the following to some lower probabilities:
      randomizeConfig("second", 0.2);
      randomizeConfig("timeZoneName", 0.2);
      randomizeConfig("weekday", 0.2);
      randomizeConfig("era", 0.1);

      // Measure the constructor.
      measureConstructor.start();
      const formatter = Intl.DateTimeFormat(locale, config);
      // Also include one format operation to ensure the constructor is de-lazified.
      formatter.format(date);
      measureConstructor.stop();

      benchmarkFormatOperation(formatter);
    }
  }

  const start = Date.UTC(2000);
  const end = Date.UTC(2030);
  const dateDiff = end - start;
  function benchmarkFormatOperation(formatter) {
    // Measure the format operation.
    for (let j = 0; j < 100; j++) {
      date = new Date(start + prng() * dateDiff);
      measureFormat.start();
      formatter.format(date);
      measureFormat.stop();
    }
  }

  benchmarkDateTimeFormatConstructor();
  measureConstructor.reportMetrics();
  measureFormat.reportMetrics();

  ok(true);
});
