/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const FIREFOX_RELEASE_TIMESTAMP = 1032800850000;
const FIREFOX_RELEASE_DATE = new Date(FIREFOX_RELEASE_TIMESTAMP);

add_task(function test_date_time_format() {
  const bundle = new FluentBundle(["en-US"]);

  bundle.addResource(
    new FluentResource(`
dt-arg = Just the arg is: {$dateArg}
dt-bare = The bare date is: { DATETIME($dateArg) }
dt-month-year = Months and year are not time-zone dependent here: { DATETIME($dateArg, month: "long") }
dt-bad = This is a bad month: { DATETIME($dateArg, month: "oops") }
# TODO - Bug 1707728:
dt-timezone = The timezone: { DATETIME($dateArg, timezone: "America/New_York") }
dt-unknown = Unknown: { DATETIME($dateArg, unknown: "unknown") }
dt-style = Style formatting: { DATETIME($dateArg, dateStyle: "short", timeStyle: "short") }
    `)
  );

  function testMessage(id, dateArg, expectedMessage) {
    const message = bundle.formatPattern(bundle.getMessage(id).value, {
      dateArg,
    });

    if (typeof expectedMessage === "object") {
      // Assume regex.
      ok(
        expectedMessage.test(message),
        `"${message}" matches regex: ${expectedMessage.toString()}`
      );
    } else {
      // Assume string.
      equal(message, expectedMessage);
    }
  }

  // TODO - Bug 1707728 - Some of these are implemented as regexes since time zones are not
  // supported in fluent messages as of yet. They could be simplified if a time zone were
  // specified.
  testMessage(
    "dt-arg",
    FIREFOX_RELEASE_DATE,
    /^Just the arg is: (Sun|Mon|Tue) Sep \d+ 2002 \d+:\d+:\d+ .* \(.*\)$/
  );
  testMessage(
    "dt-bare",
    FIREFOX_RELEASE_TIMESTAMP,
    /^The bare date is: Sep \d+, 2002, \d+:\d+:\d+ (AM|PM)$/
  );
  testMessage(
    "dt-month-year",
    FIREFOX_RELEASE_TIMESTAMP,
    "Months and year are not time-zone dependent here: September"
  );
  testMessage(
    "dt-bad",
    FIREFOX_RELEASE_TIMESTAMP,
    /^This is a bad month: Sep \d+, 2002, \d+:\d+:\d+ (AM|PM)$/
  );
  testMessage(
    "dt-unknown",
    FIREFOX_RELEASE_TIMESTAMP,
    /^Unknown: Sep \d+, 2002, \d+:\d+:\d+ (AM|PM)$/
  );
  testMessage(
    "dt-style",
    FIREFOX_RELEASE_TIMESTAMP,
    /^Style formatting: \d+\/\d+\/\d+, \d+:\d+ (AM|PM)$/
  );

// TODO - Bug 1707728
// testMessage("dt-timezone", ...);
});
