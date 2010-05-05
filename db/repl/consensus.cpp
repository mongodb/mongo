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

#include "pch.h"
#include "replset.h"

namespace mongo { 

    bool ReplSet::aMajoritySeemsToBeUp() const {
        Member *m = head();
        unsigned vTot = 0;
        unsigned vUp = 0;
        do {
            vTot += m->config().votes;
            if( m->up() )
                vTot += m->config().votes;
            m = m->next();
        } while( m );
        return vUp * 2 > vTot;
    }

    void ReplSet::electSelf() { 

    }

}
