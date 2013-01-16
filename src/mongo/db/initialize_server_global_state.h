/**
 *    Copyright (C) 2008 10gen Inc.
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

namespace mongo {

    /**
     * Perform initialization activity common across all mongo server types.
     *
     * Set up logging, daemonize the process, configure SSL, etc.
     *
     * If isMongodShutdownSpecialCase, perform this processing knowing that
     * we're only bringing this process up to kill another mongod.
     *
     * TODO: Untie the knot that requires the isMongodShutdownSpecialCase parameter.
     */
    bool initializeServerGlobalState(bool isMongodShutdownSpecialCase = false);

    void setupCoreSignals();

}  // namespace mongo
