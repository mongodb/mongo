// @file d_background_splitter.h

/**
*    Copyright (C) 2010 10gen Inc.
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

#include "../util/background.h"

namespace mongo {

    /**
     * Traverses continuously this shard's chunk and splits the ones that are above the 
     * maximum desired size
     */
    class Splitter : public BackgroundJob {
    public:
        Splitter();
        virtual ~Splitter();

        // BackgroundJob methods

        virtual void run();

        virtual string name() const { return "BackgroundSplitter"; }
    };

} // namespace mongo    
  
