/*
 *    Copyright (C) 2013 10gen Inc.
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

namespace mongo {

    // TODO: When the following calls are removed from parameters.cpp, we can remove these.
    // See SERVER-10515.

    bool isJournalingEnabled() { return false; }

    void setJournalCommitInterval(unsigned newValue) {
        // This is only for linking and should not get called at runtime
    }

    unsigned getJournalCommitInterval() {
        return 0;
    }

} // namespace mongo
