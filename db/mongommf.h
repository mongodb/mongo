/** @file mongommf.h
*
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

/* this file adds some of our layers atop memory mapped files - specifically our handling of private views & such 
   if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, not this.
*/

#pragma once

#include "../util/mmap.h"

namespace mongo {

    class MongoMMF { 
    public:
        unsigned long long length() const;
    };

}
