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

#include "mongo/db/jsobj.h"
#include "mongo/util/background.h"

/**
 * Forks off a thread to build an index.
 */
namespace mongo {

    class IndexBuilder : public BackgroundJob {
    public:
        IndexBuilder(const std::string ns, const BSONObj index);
        virtual ~IndexBuilder();

        virtual void run();
        virtual std::string name() const;

        /**
         * Kill all in-progress indexes matching criteria and, optionally, store them in the
         * indexes list.
         */
        static std::vector<BSONObj> killMatchingIndexBuilds(const BSONObj& criteria);

        /**
         * Retry all index builds in the list. Builds each index in a separate thread. If ns does
         * not match the ns field in the indexes list, the BSONObj's ns field is changed before the
         * index is built (to handle rename).
         */
        static void restoreIndexes(const std::string& ns, const std::vector<BSONObj>& indexes);

    private:
        const std::string _ns;
        const BSONObj _index;
        std::string _name;
        static AtomicUInt _indexBuildCount;
    };

}
