// lasterror.h

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

#pragma once

#include <boost/thread/tss.hpp>

namespace mongo {

    struct LastError {
        string msg;
        int nPrev;
        void raiseError(const char *_msg) {
            msg = _msg;
            nPrev = 1;
        }
        bool haveError() const {
            return !msg.empty();
        }
        void resetError() {
            msg.clear();
        }
        LastError() {
            nPrev = 0;
        }
    };

    extern boost::thread_specific_ptr<LastError> lastError;

    inline void raiseError(const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            DEV log() << "warning: lastError==0 can't report:" << msg << '\n';
            return;
        }
        le->raiseError(msg);
    }

} // namespace mongo
