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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/namespace_details.h"
#include "mongo/util/background.h"

namespace mongo {

    // This is a job that's only run at startup. It finds all incomplete indices and 
    // finishes rebuilding them. After they complete rebuilding, the thread terminates. 
    class IndexRebuilder : public BackgroundJob {
    public:
        IndexRebuilder();

        std::string name() const;
        void run();

    private:
        /**
         * Check each collection in the passed in vector to see if it has any in-progress index
         * builds that need to be retried.  If so, calls retryIndexBuild.
         */
        void checkNS(const std::vector<std::string>& nsToCheck);

        /**
         * Actually retry an index build on a given namespace.
         * @param dbName the name of the database for accessing db.system.indexes
         * @param nsd the namespace details of the namespace building the index
         * @param index the offset into nsd's index array of the partially-built index
         */
        void retryIndexBuild(const std::string& dbName,
                             NamespaceDetails* nsd );
    };

    extern IndexRebuilder indexRebuilder;
}
