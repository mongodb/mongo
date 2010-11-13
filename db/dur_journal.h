// @file dur_journal.h

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

namespace mongo {
    namespace dur {

#pragma pack(1)
        struct JSectHeader { 
            unsigned long long reserved;
        };

        struct JSectFooter { 
            unsigned long long reserved;
        };

        struct JDbContext { 
            unsigned zero;
            char dbname[1];
        };

        struct JEntry {
            unsigned len;
            short file;
        };
#pragma pack()

    }
}
