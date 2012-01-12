/** @file json.h */

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

#include "../pch.h"
#include "jsobj.h"

namespace mongo {

    /** Create a BSONObj from a JSON <http://www.json.org> string.  In addition
     to the JSON extensions extensions described here
     <http://mongodb.onconfluence.com/display/DOCS/Mongo+Extended+JSON>,
     this function accepts certain unquoted field names and allows single quotes
     to optionally be used when specifying field names and string values instead
     of double quotes.  JSON unicode escape sequences (of the form \uXXXX) are
     converted to utf8.
     \throws MsgAssertionException if parsing fails.  The message included with
     this assertion includes a rough indication of where parsing failed.
    */
    BSONObj fromjson(const string &str);

    /** len will be size of JSON object in text chars. */
    BSONObj fromjson(const char *str, int* len=NULL);

} // namespace mongo
