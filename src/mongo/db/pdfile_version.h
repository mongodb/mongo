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

#pragma once

namespace mongo {

    // pdfile versions
    const int PDFILE_VERSION = 4;
    const int PDFILE_VERSION_MINOR_22_AND_OLDER = 5;
    const int PDFILE_VERSION_MINOR_24_AND_NEWER = 6;

    // For backward compatibility with versions before 2.4.0 all new DBs start
    // with PDFILE_VERSION_MINOR_22_AND_OLDER and are converted when the first
    // index using a new plugin is created. See the logic in
    // prepareToBuildIndex() and upgradeMinorVersionOrAssert() for details

} // namespace mongo
