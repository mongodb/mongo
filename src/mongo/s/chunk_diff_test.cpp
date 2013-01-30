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

#include <string>
#include <utility>

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::ConfigDiffTracker;
    using std::string;
    using std::pair;
    using std::make_pair;

    // XXX
    // We'd move ChunkDiffUnitTest here
    // We can check the queries it generates.
    // We can check if is populating the attaching structures properly
    //

    // The default pass-through adapter for using config diffs.
    class DefaultDiffAdapter : public ConfigDiffTracker<BSONObj,string> {
    public:

        DefaultDiffAdapter() {}
        virtual ~DefaultDiffAdapter() {}

        virtual bool isTracked(const BSONObj& chunkDoc) const { return true; }
        virtual BSONObj maxFrom(const BSONObj& max) const { return max; }

        virtual pair<BSONObj,BSONObj> rangeFor(const BSONObj& chunkDoc,
                                               const BSONObj& min,
                                               const BSONObj& max) const {
            return make_pair(min, max);
        }

        virtual string shardFor(const string& name) const { return name; }
        virtual string nameFrom(const string& shard) const { return shard; }
    };

    TEST(Basics, Simple) {
        DefaultDiffAdapter differ;
        ASSERT_TRUE(true);
    }

} // unnamed namespace
