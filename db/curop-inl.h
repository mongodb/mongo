// @file curop-inl.h

namespace mongo { 

    // todo : move more here

    inline CurOp::CurOp( Client * client , CurOp * wrapped ) { 
        _client = client;
        _wrapped = wrapped;
        if ( _wrapped )
            _client->_curOp = this;
        _start = _checkpoint = 0;
        _active = false;
        _reset();
        _op = 0;
        // These addresses should never be written to again.  The zeroes are
        // placed here as a precaution because currentOp may be accessed
        // without the db mutex.
        memset(_ns, 0, sizeof(_ns));
    }

}
