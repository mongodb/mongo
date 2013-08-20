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

    class ParsedQuery;
    class LiteParsedQuery;

    // Check to see if slaveOk reads are allowed,
    // based on read preference and query options

    // DEPRECATED impl
    void replVerifyReadsOk(const ParsedQuery* pq = 0);

    void replVerifyReadsOk(const LiteParsedQuery* pq);
}
