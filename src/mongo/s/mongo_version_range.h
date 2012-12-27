/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * The MongoVersionRange represents a min/max of MongoDB versions, useful for
     * excluding/including particular versions.
     *
     * The ranges may be single-version, in which case maxVersion == "", where only exact prefix
     * matches are included in the range.  Alternately, the range may have a min and max version
     * and include any version with a prefix of the min and max version as well as all versions
     * between the two.
     */
    struct MongoVersionRange {

        static bool parseBSONArray(const BSONArray& arr,
                                   std::vector<MongoVersionRange>* excludes,
                                   std::string* errMsg);

        static BSONArray toBSONArray(const std::vector<MongoVersionRange>& ranges);

        bool parseBSONElement(const BSONElement& el, std::string* errMsg);

        void toBSONElement(BSONArrayBuilder* barr) const;

        bool isInRange(const StringData& version) const;

        std::string minVersion;
        std::string maxVersion;
    };

    bool isInMongoVersionRanges(const StringData& version,
                                const std::vector<MongoVersionRange>& ranges);
}
