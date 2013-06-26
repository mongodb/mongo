/**
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

namespace mongo {

    /**
     * Returns true if the config servers have the same contents since the last check
     * was performed. Currently checks only the config.chunks and config.databases.
     */
    bool isConfigServerConsistent();

    /**
     * Starts the thread that periodically checks data consistency amongst the config servers.
     * Note: this is not thread safe.
     */
    bool startConfigServerChecker();
}

