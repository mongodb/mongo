/**
 * Copyright (c) 2011 10gen Inc.
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
#include "util/systeminfo.h"

#include <unistd.h>

namespace mongo {

    size_t SystemInfo::getPhysicalRam() {
	/*
	  The value of this should not be changing while the system is running,
	  so it should be safe to do this once for the lifetime of the
	  application.

	  This could present a race condition if multiple threads do this at
	  the same time, but all paths through here will produce the same
	  result, so it's not worth locking or worrying about it.
	 */
	static bool unknown = true;
	static size_t ramSize = 0;

	if (unknown) {
	    long pages = sysconf(_SC_PHYS_PAGES);
	    long page_size = sysconf(_SC_PAGE_SIZE);
	    ramSize = pages * page_size;
	    unknown = false;
	}

	return ramSize;
    }

}
