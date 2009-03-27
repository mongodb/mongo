// lasterror.cpp

#include "stdafx.h"

#include "lasterror.h"

#include "jsobj.h"

namespace mongo {

    boost::thread_specific_ptr<LastError> lastError;
    LastError LastError::noError;
    
    void LastError::appendSelf( BSONObjBuilder &b ) {
        if ( !valid ) {
            b.appendNull( "err" );
            b.append( "n", 0 );
            return;
        }
        if ( msg.empty() )
            b.appendNull( "err" );
        else
            b.append( "err", msg );
        if ( updatedExisting != NotUpdate )
            b.appendBool( "updatedExisting", updatedExisting == True );
        b.append( "n", nObjects );
    }
    
} // namespace mongo
