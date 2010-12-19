/* @file dur_commitjob.cpp */

/**
*    Copyright (C) 2009 10gen Inc.
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
#include "dur_commitjob.h"

namespace mongo {
    namespace dur {

        void Writes::clear() { 
            _alreadyNoted.clear();
            _basicWrites.clear();
            _appendOps.clear();
            _ops.clear();
        }

        /** note an operation other than a "basic write" */
        void CommitJob::noteOp(shared_ptr<DurOp> p) {
            DEV dbMutex.assertWriteLocked();
            dassert( cmdLine.dur );
            if( !_hasWritten ) {
                assert( !dbMutex._remapPrivateViewRequested );
                _hasWritten = true;
            }
            _wi._ops.push_back(p);
        }

        void CommitJob::reset() { 
            _hasWritten = false;
            _wi.clear();
            _ab.reset();
            _bytes = 0;
        }
    }
}
