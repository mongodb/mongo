// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace sbe {
namespace vm {

/**
 * Returns true if a timezone represented by 'timezone' is valid and exists in database
 * 'timezoneDB', meaning that it can be retrieved by function 'getTimezone()'.
 */
bool isValidTimezone(value::TagValueView timezone, const TimeZoneDatabase* timezoneDB);

/**
 * Returns a TimeZone object representing the zone given by 'timezone', or UTC if it was an empty
 * string.
 */
TimeZone getTimezone(value::TagValueView timezone, TimeZoneDatabase* timezoneDB);

/**
 * Returns a Date_t object representing the datetime given by 'date'.
 */
Date_t getDate(value::TagValueView date);

/**
 * Returns 'true' if a value of type encoded by 'typeTag' can be coerced to Date_t by function
 * 'getDate()', otherwise returns false.
 */
bool coercibleToDate(value::TypeTags typeTag);
}  // namespace vm

}  // namespace sbe
}  // namespace mongo
