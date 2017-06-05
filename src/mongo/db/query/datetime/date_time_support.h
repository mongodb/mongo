/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ServiceContext;

/**
 * A C++ interface wrapping the third-party timelib library. A single instance of this class can be
 * accessed via the global service context.
 */
class DateTimeSupport {
    MONGO_DISALLOW_COPYING(DateTimeSupport);

public:
    /**
     * A custom-deleter which deletes 'timeZoneDatabase' if it is not the builtin time zone
     * database, which has static lifetime and should not be freed.
     */
    struct TimeZoneDBDeleter {
        TimeZoneDBDeleter() = default;
        void operator()(timelib_tzdb* timeZoneDatabase);
    };

    /**
     * Creates a DateTimeSupport object with time zone data loaded from timelib's built-in timezone
     * rules.
     */
    DateTimeSupport();

    /**
     * Creates a DateTimeSupport object using time zone rules given by 'timeZoneDatabase'.
     */
    DateTimeSupport(std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

    ~DateTimeSupport();

    /**
     * Returns the DateTimeSupport object associated with the specified service context. This method
     * must only be called if a DateTimeSupport has been set on the service context.
     */
    static const DateTimeSupport* get(ServiceContext* serviceContext);

    /**
     * Sets the DateTimeSupport object associated with the specified service context.
     */
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<DateTimeSupport> dateTimeSupport);

private:
    /**
     * Populates '_timeZones' with parsed time zone rules for each timezone specified by
     * 'timeZoneDatabase'.
     */
    void loadTimeZoneInfo(std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> timeZoneDatabase);

    // A map from the time zone name to the struct describing the timezone. These are pre-populated
    // at startup to avoid reading the source files repeatedly.
    StringMap<timelib_tzinfo*> _timeZones;

    // The timelib structure which provides timezone information.
    std::unique_ptr<timelib_tzdb, TimeZoneDBDeleter> _timeZoneDatabase;
};

}  // namespace mongo
