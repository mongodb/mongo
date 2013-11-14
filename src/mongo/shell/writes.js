//
// Javascript shell utilities related to new-style write operations
//

if ( ( typeof WriteConcern ) == 'undefined' ){

    /**
     * Shell representation of WriteConcern, includes:
     *  j: write durably written to journal
     *  w: write replicated to number of servers
     *  wtimeout: how long to wait for replication
     * 
     * Accepts { w : x, j : x, wtimeout : x } or w, j, wtimeout
     */
    WriteConcern = function( wValue, jValue, wTimeout ){

        if ( typeof wValue == 'object' && !jValue ) {
            var opts = wValue;
            wValue = opts.w;
            jValue = opts.j;
            wTimeout = opts.wtimeout;
        }

        this._w = wValue;
        if ( this._w === undefined ) this._w = 1;
        assert( typeof this._w == 'number' || typeof this._w == 'string' );

        this._j = jValue ? true : false;
        this._wTimeout = NumberInt( wTimeout ).toNumber();
    };

    WriteConcern.prototype.tojson = function() {
        return { w : this._w, j : this._j, wtimeout : this._wTimeout };
    };

    WriteConcern.prototype.toString = function() {
        return "WriteConcern(" + tojson( this.tojson() ) + ")";
    };

    WriteConcern.prototype.shellPrint = function() {
        return this.toString();
    };
}

if ( ( typeof WriteResult ) == 'undefined' ){
    
    /**
     * WriteResults represent the output of batch write operations.
     * 
     * Results may either be 'ok' or not, and have a number of documents modified along with
     * any upserted ids per-batch-item.
     * 
     * If a WriteResult is not ok, it may have one of three error types:
     * - Batch error, implying no writes occurred
     * - Item errors, indexed per-item if a batch error did not occur
     * - WriteConcern error, if the writes that succeeded during the batch could not be persisted to
     * the necessary level
     * 
     * All errors have the form:
     *  { code : <number>, errmsg : <string>, [errInfo : <obj>, index : <number>] }
     * where errInfo and index are optional.  Depending on the error type, errInfo may have
     * additional information.
     * 
     * If the batch is of size 1, the getSingleError() function will return either the applicable
     * batch error or the item error.
     */
    WriteResult = function( opType, result ){

        this._result = result;

        if ( opType ) this._opType = opType;
        else this._opType = undefined;
        assert( opType == 'insert' || opType == 'update' ||
                opType == 'remove' || opType == undefined );

        this._ok = result.ok ? true : false;

        if ( !this._ok ) {

            result.code |= WriteResult.getUnknownErrorCode();

            var topLevelError = { code : NumberInt( result.code ).toNumber(),
                                  errmsg : result.errmsg.toString(),
                                  errInfo : result.errInfo || {} };
            assert.eq( typeof topLevelError.errInfo, 'object' );

            var isWCError = this._errCode == WriteResult.getWCErrorCode();
            if ( isWCError ) {
                // WC Error
                this._wcError = topLevelError;
            }

            if ( result.errDetails ) {
                // Item errors
                this._itemErrors = result.errDetails;
                assert( Array.isArray( this._itemErrors ) );
            }
            else if ( !isWCError ) {
                // Batch error
                this._batchError = topLevelError;
            }

            assert( !( '_itemErrors' in this && '_batchError' in this ) );
        }

        this._n = result.n ? NumberInt( result.n ).toNumber() : 0;
        this._upsertedIds = result.upserted;
        if ( !Array.isArray( this._upsertedIds ) ) {
            this._upsertedIds = [{ index : 0, upsertedId : result.upserted }];
        }
    };

    WriteResult.getUnknownErrorCode = function() {
        return 8; // UnknownError error code
    };

    WriteResult.getWCErrorCode = function() {
        return 64; // WriteConcernFailed error code
    };

    WriteResult.prototype.tojson = function() {
        return this._result;
    };

    WriteResult.prototype.toString = function() {
        var opTypeStr = this.getOpTypeString();
        return opTypeStr.charAt(0).toUpperCase() + opTypeStr.slice(1) + " " +
               "WriteResult(" + tojson( this._result )+ ")";
    };

    WriteResult.prototype.getOpTypeString = function() {
        if ( this._opType ) return this._opType;
        return 'unknown';
    };

    WriteResult.prototype.shellPrint = function() {
        return this.toString();
    };

    WriteResult.prototype.isOK = function() {
        return this._ok;
    };

    WriteResult.prototype.getWCError = function() {
        return this._wcError;
    };

    WriteResult.prototype.getBatchError = function() {
        return this._batchError;
    };

    WriteResult.prototype.numItemErrors = function() {
        return '_itemErrors' in this ? this._itemErrors.length : 0;
    };

    WriteResult.prototype.getItemError = function( itemIndex ) {
        return this._itemErrors[itemIndex];
    };

    WriteResult.prototype.getSingleError = function() {
        // There's no internal way to distinguish between batch errors and single errors
        if ( this.getBatchError() ) return this.getBatchError();
        if ( this.numItemErrors() > 0 ) return this.getItemError( 0 );
        return null;
    };

    WriteResult.prototype.getNumModified = function() {
        return this._n;
    };

    WriteResult.prototype.getUpsertedId = function( itemIndex ) {
        return this._upsertedIds[itemIndex].upsertedId;
    };

    WriteResult.prototype.getSingleUpsertedId = function() {
        return this.getUpsertedId( 0 );
    };
    
}

