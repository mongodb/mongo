/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

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
