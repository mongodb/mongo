// @file pagefault.cpp

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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/


#include "mongo/pch.h"

#include "mongo/db/pagefault.h"

#include "mongo/db/client.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/pdfile.h"
#include "mongo/server.h"

namespace mongo { 

    PageFaultException::PageFaultException(const Record *_r)
    {
        verify( cc().allowedToThrowPageFaultException() );
        cc().getPageFaultRetryableSection()->didLap();
        r = _r;
        era = LockMongoFilesShared::getEra();
        LOG(2) << "PageFaultException thrown" << endl;
    }

    void PageFaultException::touch() { 
        if ( Lock::isLocked() ) {
            warning() << "PageFaultException::touch happening with a lock" << endl;
        }
        LockMongoFilesShared lk;
        if( LockMongoFilesShared::getEra() != era ) {
            // files opened and closed.  we don't try to handle but just bail out; this is much simpler
            // and less error prone and saves us from taking a dbmutex readlock.
            MONGO_DLOG(2) << "era changed" << endl;
            return;
        }
        r->touch();
    }

    PageFaultRetryableSection::~PageFaultRetryableSection() {
        cc()._pageFaultRetryableSection = 0;
    }
    PageFaultRetryableSection::PageFaultRetryableSection() {
        _laps = 0;
        verify( cc()._pageFaultRetryableSection == 0 );
        if( Lock::isLocked() ) {
            cc()._pageFaultRetryableSection = 0;
            if( debug || logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(3)) ) { 
                LOGSOME << "info PageFaultRetryableSection will not yield, already locked upon reaching" << endl;
            }
        }
        else {
            cc()._pageFaultRetryableSection = this;
            cc()._hasWrittenThisPass = false;
        }
    }


    NoPageFaultsAllowed::NoPageFaultsAllowed() {
        _saved = cc()._pageFaultRetryableSection;
        cc()._pageFaultRetryableSection = 0;
    }

    NoPageFaultsAllowed::~NoPageFaultsAllowed() {
        cc()._pageFaultRetryableSection = _saved;
    }
}
