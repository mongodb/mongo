/**
 * Copyright (c) 2012 10gen Inc.
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
 */

#include "pch.h"

#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/document_source.h"

namespace mongo {

    DependencyTracker::Tracker::Tracker(
        const string &fp, const DocumentSource *pS):
        fieldPath(fp),
        pSource(pS) {
    }

    void DependencyTracker::addDependency(
        const string &fieldPath, const DocumentSource *pSource) {
        Tracker tracker(fieldPath, pSource);
        std::pair<MapType::iterator, bool> p(
            map.insert(std::pair<string, Tracker>(fieldPath, tracker)));

        /*
          If there was already an entry, update the dependency to be the more
          recent source.
        */
        if (!p.second)
            (*p.first).second.pSource = pSource;
    }

    void DependencyTracker::removeDependency(const string &fieldPath) {
        map.erase(fieldPath);
    }

    bool DependencyTracker::getDependency(
        intrusive_ptr<const DocumentSource> *ppSource,
        const string &fieldPath) const {
        MapType::const_iterator i(map.find(fieldPath));
        if (i == map.end())
            return false;

        *ppSource = (*i).second.pSource;
        return true;
    }

}
