// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Golden change-detector tests for the vendored timelib library and its compiled-in tzdata.
 *
 * These tests are not correctness oracles. They snapshot the current behavior of the parser,
 * date arithmetic, date parts, formatting and the timezone database, so that a timelib or
 * tzdata upgrade produces a plain-text diff in the PR that performs the upgrade.
 *
 * All inputs are absolute Date_t values or strings carrying a full date; nothing reads the
 * system clock or the host's timezone data. Strings that only carry a zone (e.g. "EST") are
 * deliberately excluded because parsing them would consult the current date.
 *
 * To regenerate the golden files after an intentional timelib/tzdata update, run this test and
 * then `python3 buildscripts/golden_test.py accept` (see docs/golden_data_test_framework.md).
 */

#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/query/datetime/expected_output"};

const TimeZoneDatabase kTimeZoneDatabase{};
const TimeZone kUtc = TimeZoneDatabase::utcZone();
const TimeZone kNewYork = kTimeZoneDatabase.getTimeZone("America/New_York");

// Renders an input string so that every case stays on one golden-file line.
std::string escaped(std::string_view s) {
    std::string out;
    char buf[5];
    for (unsigned char c : s) {
        switch (c) {
            case '\0':
                out += "\\0";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    snprintf(buf, sizeof(buf), "\\x%02X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Milliseconds plus ISO form; the millis keep the value unambiguous where the ISO rendering
// itself cannot represent the date (years outside 0-9999 report error 18537).
std::string showDate(Date_t d) {
    auto str = kUtc.formatDate(kIsoFormatStringZ, d);
    return str.isOK()
        ? fmt::format("{} [{}]", d.toMillisSinceEpoch(), str.getValue())
        : fmt::format("{} [ERROR {}]", d.toMillisSinceEpoch(), str.getStatus().code());
}

std::string showError(const AssertionException& e) {
    auto status = e.toStatus();
    // Escape the reason: error messages quote the input verbatim, which may contain NUL or
    // invalid UTF-8, and raw control bytes would make the golden files binary.
    return fmt::format("ERROR {}: {}", status.codeString(), escaped(status.reason()));
}

Date_t utcDate(long long year, long long month, long long day) {
    return kUtc.createFromDateParts(year, month, day, 0, 0, 0, 0);
}

TEST(TimelibGolden, Parser) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    const std::vector<std::string> inputs = {
        // Whitespace.
        "",
        " ",
        "\t",
        "  2024-01-01",
        "2024-01-01  ",
        "2024-01-01  10:11:12",
        // Zone syntax.
        "2024-01-01T00:00:00 (UTC)",
        "2024-01-01T00:00:00 UTC)",
        "2024-01-01T00:00:00 ((EST)",
        "2024-01-01T00:00:00+0530",
        "2024-01-01T00:00:00+05:30",
        "2024-01-01T00:00:00Z",
        "2024-01-01 00:00:00 GMT+1",
        "2024-01-01 EST",
        "2024-01-01 CEST",
        // Date forms.
        "2024-03-01",
        "2024-03-01T10:11:12",
        "2024-03-01T10:11:12.123Z",
        "20240301",
        "2024-03-01 10:11:12",
        "2024-W09-5",
        "2024W095",
        "2024-061",
        "2024061",
        "2024-02-30",
        "2024-13-01",
        "0000-01-01",
        "-0001-01-01",
        "03/01/2024",
        // Garbage.
        "2024-01-01xyz",
        "not a date",
        "2024-01-01\xC3\xA9",                                      // U+00E9 in UTF-8.
        "\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x92\xEF\xBC\x94-01-01",  // Full-width "2024".
    };

    auto run = [&](std::string_view label, std::string_view input) {
        os << label << " -> ";
        try {
            os << showDate(kTimeZoneDatabase.fromString(input, kUtc));
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    };

    for (auto&& input : inputs) {
        run(escaped(input), input);
    }
    // Inputs whose label cannot be the escaped input itself.
    run("embedded NUL", std::string("2024-01-01\0junk", 15));
    run("'2024-01-01' + 10k spaces", "2024-01-01" + std::string(10'000, ' '));
}

TEST(TimelibGolden, ParserFormats) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    const std::vector<std::pair<std::string_view, std::string>> cases = {
        {"%Y-%m-%d", "2024-03-01"},
        {"%Y-%m-%d", "2024-3-1"},
        {"%Y-%m-%d", "2024-03-01junk"},
        {"%Y-%m-%d", "24-03-01"},
        {"%Y-%m-%d", "2024-13-01"},
        {"%Y-%m-%d", "0000-01-01"},
        {"%Y-%m-%dT%H:%M:%S", "2024-03-01T10:11:12"},
        {"%Y-%m-%d %H:%M:%S.%L", "2024-03-01 10:11:12.123"},
        {"%Y-%m-%d %z", "2024-03-01 +0530"},
        {"%Y-%m-%d %z", "2024-03-01 +05:30"},
        {"%Y-%m-%d %z", "2024-03-01 -0000"},
        {"%Y-%m-%d %z", "2024-03-01 Z"},
        {"%Y-%m-%d %Z", "2024-03-01 UTC"},
        {"%Y-%m-%d %Z", "2024-03-01 EST"},
        {"%Y-%m-%d %Z", "2024-03-01 CEST"},
        {"%Y-%m-%d %Z", "2024-03-01 GMT+1"},
        {"%G-W%V-%u", "2024-W09-5"},
        {"%G-W%V-%u", "2019-W52-7"},
        {"%G-W%V-%u", "2020-W53-7"},
        {"%Y-%j", "2024-061"},
        {"%Y-%j", "2023-365"},
        {"%Y-%j", "2024-366"},
        {"%d/%m/%Y", "01/03/2024"},
    };

    for (auto&& [format, input] : cases) {
        os << escaped(format) << " | " << escaped(input) << " -> ";
        try {
            os << showDate(kTimeZoneDatabase.fromString(input, kUtc, format));
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    }
}

TEST(TimelibGolden, Arithmetic) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    const std::vector<TimeUnit> units = {
        TimeUnit::year, TimeUnit::quarter, TimeUnit::month, TimeUnit::week, TimeUnit::day};

    auto add = [&](std::string_view tzLabel,
                   const TimeZone& tz,
                   std::string_view baseLabel,
                   long long year,
                   long long month,
                   long long day,
                   TimeUnit unit,
                   long long amount) {
        os << fmt::format(
            "dateAdd({}, {}, {}, {:+}) -> ", tzLabel, baseLabel, serializeTimeUnit(unit), amount);
        try {
            auto date = tz.createFromDateParts(year, month, day, 0, 0, 0, 0);
            os << showDate(dateAdd(date, unit, amount, tz));
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    };

    // UTC: edge years, leap days, month-end clamping, negative-epoch boundary.
    const std::vector<std::pair<std::string_view, std::tuple<long long, long long, long long>>>
        utcBases = {
            {"-0001-03-01", {-1, 3, 1}},
            {"0000-02-29", {0, 2, 29}},
            {"0001-03-01", {1, 3, 1}},
            {"9999-12-31", {9999, 12, 31}},
            {"2024-02-29", {2024, 2, 29}},
            {"2023-02-28", {2023, 2, 28}},
            {"1900-02-28", {1900, 2, 28}},
            {"2000-02-29", {2000, 2, 29}},
            {"2024-01-31", {2024, 1, 31}},
            {"2024-03-31", {2024, 3, 31}},
            {"1969-12-31", {1969, 12, 31}},
            {"1970-01-01", {1970, 1, 1}},
        };
    for (auto&& [baseLabel, ymd] : utcBases) {
        auto [y, m, d] = ymd;
        for (auto unit : units) {
            for (auto amount : {1, -1}) {
                add("UTC", kUtc, baseLabel, y, m, d, unit, amount);
            }
        }
    }

    // A DST zone: spring-forward/fall-back boundaries, month-end, epoch boundary.
    const std::vector<std::pair<std::string_view, std::tuple<long long, long long, long long>>>
        nyBases = {
            {"2026-03-08", {2026, 3, 8}},
            {"2026-11-01", {2026, 11, 1}},
            {"2024-01-31", {2024, 1, 31}},
            {"1969-12-31", {1969, 12, 31}},
        };
    for (auto&& [baseLabel, ymd] : nyBases) {
        auto [y, m, d] = ymd;
        for (auto unit : units) {
            for (auto amount : {1, -1}) {
                add("America/New_York", kNewYork, baseLabel, y, m, d, unit, amount);
            }
        }
    }

    auto diff = [&](std::string_view tzLabel,
                    const TimeZone& tz,
                    std::string_view label,
                    Date_t start,
                    Date_t end,
                    TimeUnit unit) {
        os << fmt::format("dateDiff({}, {}, {}) -> ", tzLabel, label, serializeTimeUnit(unit));
        try {
            os << dateDiff(start, end, unit, tz);
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    };

    for (auto unit : units) {
        diff("UTC", kUtc, "2024-01-01..2024-03-01", utcDate(2024, 1, 1), utcDate(2024, 3, 1), unit);
    }
    diff("UTC",
         kUtc,
         "2023-02-28..2024-02-29",
         utcDate(2023, 2, 28),
         utcDate(2024, 2, 29),
         TimeUnit::year);
    diff("UTC", kUtc, "0000-01-01..0001-01-01", utcDate(0, 1, 1), utcDate(1, 1, 1), TimeUnit::day);
    diff(
        "UTC", kUtc, "-0001-01-01..0001-01-01", utcDate(-1, 1, 1), utcDate(1, 1, 1), TimeUnit::day);
    diff("UTC",
         kUtc,
         "1969-12-31..1970-01-01",
         utcDate(1969, 12, 31),
         utcDate(1970, 1, 1),
         TimeUnit::day);
    // The same wall-clock span across the New York DST gap, in both zones.
    auto nyGapStart = kNewYork.createFromDateParts(2026, 3, 8, 1, 0, 0, 0);
    auto nyGapEnd = kNewYork.createFromDateParts(2026, 3, 8, 4, 0, 0, 0);
    diff("America/New_York",
         kNewYork,
         "2026-03-08 01:00..04:00",
         nyGapStart,
         nyGapEnd,
         TimeUnit::day);
    diff("America/New_York",
         kNewYork,
         "2026-03-08 01:00..04:00",
         nyGapStart,
         nyGapEnd,
         TimeUnit::hour);
    diff("UTC", kUtc, "2026-03-08 06:00..08:00", nyGapStart, nyGapEnd, TimeUnit::hour);
}

TEST(TimelibGolden, DateParts) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto showParts = [](const TimeZone::DateParts& p) {
        return fmt::format("{}-{}-{} {}:{}:{}.{}",
                           p.year,
                           p.month,
                           p.dayOfMonth,
                           p.hour,
                           p.minute,
                           p.second,
                           p.millisecond);
    };

    // Round trips at boundary years (incl. year 0 / negative years) and the epoch boundary.
    auto parts = [&](std::string_view tzLabel,
                     const TimeZone& tz,
                     long long y,
                     long long m,
                     long long d,
                     long long h = 0,
                     long long min = 0,
                     long long s = 0,
                     long long ms = 0) {
        os << fmt::format(
            "dateFromParts({}, {}-{}-{} {}:{}:{}.{}) -> ", tzLabel, y, m, d, h, min, s, ms);
        try {
            auto date = tz.createFromDateParts(y, m, d, h, min, s, ms);
            os << showDate(date) << " | roundtrip " << showParts(tz.dateParts(date));
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    };

    for (auto [y, m, d] : {std::tuple{-1LL, 3LL, 1LL},
                           {0LL, 1LL, 1LL},
                           {0LL, 2LL, 29LL},
                           {1LL, 1LL, 1LL},
                           {9999LL, 12LL, 31LL},
                           {2024LL, 2LL, 29LL},
                           {1969LL, 12LL, 31LL},
                           {1970LL, 1LL, 1LL}}) {
        parts("UTC", kUtc, y, m, d);
    }
    // Out-of-range parts.
    parts("UTC", kUtc, 2024, 0, 15);
    parts("UTC", kUtc, 2024, 13, 15);
    parts("UTC", kUtc, 2024, 3, 0);
    parts("UTC", kUtc, 2024, 3, 32);
    parts("UTC", kUtc, 2024, 3, 1, 25);
    // DST gap (2026-03-08 02:30 does not exist) and overlap (2026-11-01 01:30 happens twice).
    parts("America/New_York", kNewYork, 2026, 3, 8, 2, 30);
    parts("America/New_York", kNewYork, 2026, 11, 1, 1, 30);

    // ISO week-date parts, including a year without a week 53.
    auto isoParts = [&](long long isoYear, long long isoWeek, long long isoDayOfWeek) {
        os << fmt::format("dateFromIsoWeekParts({}, {}, {}) -> ", isoYear, isoWeek, isoDayOfWeek);
        try {
            auto date = kUtc.createFromIso8601DateParts(isoYear, isoWeek, isoDayOfWeek, 0, 0, 0, 0);
            auto p = kUtc.dateIso8601Parts(date);
            os << showDate(date)
               << fmt::format(" | roundtrip {}-W{}-{}", p.year, p.weekOfYear, p.dayOfWeek);
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    };
    isoParts(2024, 9, 5);
    isoParts(2019, 52, 7);
    isoParts(2020, 53, 7);
    isoParts(2021, 53, 1);
    isoParts(2024, 0, 1);
    isoParts(2024, 9, 8);
    isoParts(0, 1, 1);
    isoParts(-1, 1, 1);
}

TEST(TimelibGolden, Formatting) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    const std::vector<std::string_view> specifiers = {
        "%Y", "%y", "%m", "%d", "%e", "%H", "%I", "%k", "%L", "%M", "%p", "%S", "%j",
        "%w", "%u", "%U", "%V", "%G", "%z", "%Z", "%a", "%A", "%b", "%B", "%%",
    };
    const std::vector<std::pair<std::string_view, Date_t>> dates = {
        {"2024-03-05T14:23:17.456Z", Date_t::fromMillisSinceEpoch(1709648597456LL)},
        {"2024-03-05T09:08:07.006Z", Date_t::fromMillisSinceEpoch(1709629687006LL)},
    };
    const std::vector<std::pair<std::string_view, const TimeZone*>> zones = {
        {"UTC", &kUtc}, {"America/New_York", &kNewYork}};

    for (auto&& [zoneLabel, tz] : zones) {
        for (auto format : specifiers) {
            for (auto&& [dateLabel, date] : dates) {
                os << fmt::format("formatDate({}, {}, {}) -> ", zoneLabel, format, dateLabel);
                if (!TimeZone::isValidToStringFormat(format)) {
                    os << "INVALID_FORMAT\n";
                    continue;
                }
                auto str = tz->formatDate(format, date);
                if (str.isOK()) {
                    os << escaped(str.getValue());
                } else {
                    os << fmt::format(
                        "ERROR {}: {}", str.getStatus().codeString(), str.getStatus().reason());
                }
                os << "\n";
            }
        }
    }

    // Calendar functions at ISO week/year boundaries.
    const std::vector<std::pair<std::string_view, Date_t>> boundaryDates = {
        {"2019-12-29", utcDate(2019, 12, 29)},
        {"2019-12-30", utcDate(2019, 12, 30)},
        {"2019-12-31", utcDate(2019, 12, 31)},
        {"2020-01-01", utcDate(2020, 1, 1)},
        {"2020-12-31", utcDate(2020, 12, 31)},
        {"2021-01-01", utcDate(2021, 1, 1)},
        {"0000-01-01", utcDate(0, 1, 1)},
        {"1969-12-31", utcDate(1969, 12, 31)},
    };
    for (auto&& [label, date] : boundaryDates) {
        os << fmt::format(
            "{} -> dayOfWeek={} isoDayOfWeek={} dayOfYear={} week={} isoWeek={} isoYear={}\n",
            label,
            kUtc.dayOfWeek(date),
            kUtc.isoDayOfWeek(date),
            kUtc.dayOfYear(date),
            kUtc.week(date),
            kUtc.isoWeek(date),
            kUtc.isoYear(date));
    }
}

TEST(TimelibGolden, TimeZoneList) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    auto names = kTimeZoneDatabase.getTimeZoneStrings();
    std::sort(names.begin(), names.end());
    os << "count: " << names.size() << "\n";
    for (auto&& name : names) {
        os << name << "\n";
    }
}

TEST(TimelibGolden, TimeZoneFingerprint) {
    unittest::GoldenTestContext ctx(&goldenTestConfig);
    auto& os = ctx.outStream();

    // Fixed absolute probe schedule: the 1st and 15th of every month at 00:00 and 12:00 UTC,
    // 1970-2040, plus explicit far-future dates to exercise the POSIX-TZ extension rule.
    std::vector<Date_t> probes;
    for (int year = 1970; year <= 2040; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day : {1, 15}) {
                for (int hour : {0, 12}) {
                    probes.push_back(kUtc.createFromDateParts(year, month, day, hour, 0, 0, 0));
                }
            }
        }
    }
    for (auto d :
         {utcDate(2100, 1, 1), utcDate(2100, 7, 1), utcDate(9999, 1, 1), utcDate(9999, 7, 1)}) {
        probes.push_back(d);
    }

    // ponytail: FNV-1a over the probe offsets; offsets are the observable of DST anyway, so no
    // separate DST flag is recorded.
    const uint64_t kFnvOffset = 14695981039346656037ULL;
    const uint64_t kFnvPrime = 1099511628211ULL;

    auto names = kTimeZoneDatabase.getTimeZoneStrings();
    std::sort(names.begin(), names.end());
    for (auto&& name : names) {
        os << name << " -> ";
        try {
            auto tz = kTimeZoneDatabase.getTimeZone(name);
            uint64_t hash = kFnvOffset;
            for (auto probe : probes) {
                for (char c : fmt::format("{};", tz.utcOffset(probe).count())) {
                    hash ^= static_cast<unsigned char>(c);
                    hash *= kFnvPrime;
                }
            }
            os << fmt::format("{:016x}", hash);
        } catch (const AssertionException& e) {
            os << showError(e);
        }
        os << "\n";
    }

    // Raw transition dumps for a few key zones, so tzdata diffs stay human-readable.
    const std::vector<std::string_view> keyZones = {"America/Edmonton",
                                                    "America/Winnipeg",
                                                    "America/New_York",
                                                    "Europe/London",
                                                    "Australia/Lord_Howe",
                                                    "Pacific/Apia",
                                                    "Asia/Kolkata"};
    const auto start = utcDate(1970, 1, 1);
    const auto end = utcDate(2041, 1, 1);
    for (auto name : keyZones) {
        os << "## " << name << "\n";
        auto tz = kTimeZoneDatabase.getTimeZone(name);
        auto prev = tz.utcOffset(start).count();
        for (auto t = start + Seconds{86400}; t < end; t = t + Seconds{86400}) {
            auto offset = tz.utcOffset(t).count();
            if (offset != prev) {
                os << fmt::format(
                    "{}: {} -> {}\n", kUtc.formatDate("%Y-%m-%d", t).getValue(), prev, offset);
                prev = offset;
            }
        }
    }
}

}  // namespace
}  // namespace mongo
