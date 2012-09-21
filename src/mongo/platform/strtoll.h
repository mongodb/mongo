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

#include <cstdlib>

#ifdef _WIN32
static inline long long strtoll(const char* nptr, char** endptr, int base) {
    return _strtoi64(nptr, endptr, base);
}

static inline unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    return _strtoui64(nptr, endptr, base);
}
#endif  // defined(_WIN32)
