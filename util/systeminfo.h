/**
 * Copyright 2011 (c) 10gen Inc.
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

#pragma once

#include <cstddef>

namespace mongo {

    class SystemInfo {
    public:
	/*
	  Get the amount of physical memory available on the host.

	  This should only be used for "advisory" purposes, and not as a hard
	  value, because this could be deceptive on virtual hosts, and because
	  this will return zero on platforms that do not support it.

	  @returns amount of physical memory, or zero
	 */
	static size_t getPhysicalRam();

    private:
	// don't instantiate this class
	SystemInfo(); // no implementation
    };

}
