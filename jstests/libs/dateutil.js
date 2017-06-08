"use strict";

/**
 * Helpers for generating test dates for aggregations
 */
var DateUtil = (function() {

    /**
     * local function to add leading 0 to month or day if needed.
     */
    function padded(val) {
        return ("00" + val).slice(-2);
    }

    function getNewYear(year) {
        return new Date("" + year + "-01-01T00:00:00Z");
    }

    function getEndOfFirstWeekInYear(year, day) {
        return new Date("" + year + "-01-" + (padded(7 - day + 1)) + "T23:59:59Z");
    }

    function getStartOfSecondWeekInYear(year, day) {
        return new Date("" + year + "-01-" + (padded(7 - day + 2)) + "T00:00:00Z");
    }

    function getBirthday(year) {
        return new Date("" + year + "-07-05T21:36:00+02:00");
    }

    function getEndOfSecondToLastWeekInYear(year, day, type) {
        if (type === 'leap') {
            return new Date("" + year + "-12-" + padded(31 - day - 1) + "T23:59:59Z");
        } else {
            return new Date("" + year + "-12-" + padded(31 - day) + "T23:59:59Z");
        }
    }

    function getStartOfLastWeekInYear(year, day, type) {
        if (type === 'leap') {
            return new Date("" + year + "-12-" + padded(31 - day) + "T00:00:00Z");
        } else {
            return new Date("" + year + "-12-" + padded(31 - day + 1) + "T00:00:00Z");
        }
    }

    function getNewYearsEve(year) {
        return new Date("" + year + "-12-31T23:59:59Z");
    }

    function shiftWeekday(dayOfWeek, daysToAdd) {
        return ((dayOfWeek - 1 + daysToAdd) % 7) + 1;
    }

    return {
        getNewYear: getNewYear,
        getEndOfFirstWeekInYear: getEndOfFirstWeekInYear,
        getStartOfSecondWeekInYear: getStartOfSecondWeekInYear,
        getBirthday: getBirthday,
        getEndOfSecondToLastWeekInYear: getEndOfSecondToLastWeekInYear,
        getStartOfLastWeekInYear: getStartOfLastWeekInYear,
        getNewYearsEve: getNewYearsEve,
        shiftWeekday: shiftWeekday
    };
})();
