// @file pagefault.cpp

#include "pch.h"
#include "diskloc.h"
#include "pagefault.h"
#include "client.h"
#include "pdfile.h"
#include "server.h"

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
        verify( ! Lock::isLocked() );
        LockMongoFilesShared lk;
        if( LockMongoFilesShared::getEra() != era ) {
            // files opened and closed.  we don't try to handle but just bail out; this is much simpler
            // and less error prone and saves us from taking a dbmutex readlock.
            dlog(2) << "era changed" << endl;
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
            if( debug || logLevel > 2 ) { 
                LOGSOME << "info PageFaultRetryableSection will not yield, already locked upon reaching" << endl;
            }
        }
        else {
            cc()._pageFaultRetryableSection = this;
            cc()._hasWrittenThisPass = false;
        }
    }

}
