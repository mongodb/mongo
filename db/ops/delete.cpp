// delete.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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
#include "delete.h"
#include "../queryoptimizer.h"
#include "../oplog.h"

namespace mongo {
    
    // Just try to identify best plan.
    class DeleteOp : public MultiCursor::CursorOp {
    public:
        DeleteOp( bool justOne, int& bestCount, int orClauseIndex = -1 ) :
            justOne_( justOne ),
            count_(),
            bestCount_( bestCount ),
            _nscanned(),
            _orClauseIndex( orClauseIndex ) {
        }
        virtual void _init() {
            c_ = qp().newCursor();
        }
        virtual bool prepareToYield() {
            if ( _orClauseIndex > 0 ) {
                return false;
            }
            if ( ! _cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , c_ , qp().ns() ) );
            }
            return _cc->prepareToYield( _yieldData );
        }
        virtual void recoverFromYield() {
            if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                _cc.reset();
                c_.reset();
                massert( 13340, "cursor dropped during delete", false );
            }
        }
        virtual long long nscanned() {
            return c_.get() ? c_->nscanned() : _nscanned;
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }

            DiskLoc rloc = c_->currLoc();

            if ( matcher( c_ )->matchesCurrent(c_.get()) ) {
                if ( !c_->getsetdup(rloc) )
                    ++count_;
            }

            c_->advance();
            _nscanned = c_->nscanned();
            
            if ( _orClauseIndex > 0 && _nscanned >= 100 ) {
                setComplete();
                return;
            }
            
            if ( count_ > bestCount_ )
                bestCount_ = count_;

            if ( count_ > 0 ) {
                if ( justOne_ )
                    setComplete();
                else if ( _nscanned >= 100 && count_ == bestCount_ )
                    setComplete();
            }
        }
        virtual bool mayRecordPlan() const { return !justOne_; }
        virtual QueryOp *_createChild() const {
            bestCount_ = 0; // should be safe to reset this in contexts where createChild() is called
            return new DeleteOp( justOne_, bestCount_, _orClauseIndex + 1 );
        }
        virtual shared_ptr<Cursor> newCursor() const { return qp().newCursor(); }
    private:
        bool justOne_;
        int count_;
        int &bestCount_;
        long long _nscanned;
        shared_ptr<Cursor> c_;
        ClientCursor::CleanupPointer _cc;
        ClientCursor::YieldData _yieldData;
        // Avoid yielding in the MultiPlanScanner when not the first $or clause - just a temporary implementaiton for now.
        int _orClauseIndex;
    };

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
       god:     allow access to system namespaces, and don't yield
    */
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOneOrig, bool logop, bool god, RemoveSaver * rs ) {
        if( !god ) {
            if ( strstr(ns, ".system.") ) {
                /* note a delete from system.indexes would corrupt the db
                if done here, as there are pointers into those objects in
                NamespaceDetails.
                */
                uassert(12050, "cannot delete from system namespace", legalClientSystemNS( ns , true ) );
            }
            if ( strchr( ns , '$' ) ) {
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uassert( 10100 ,  "cannot delete from collection with reserved $ in name", strchr(ns, '$') == 0 );
            }
        }

        {
            NamespaceDetails *d = nsdetails( ns );
            if ( ! d )
                return 0;
            uassert( 10101 ,  "can't remove from a capped collection" , ! d->capped );
        }

        long long nDeleted = 0;

        int best = 0;
        shared_ptr< MultiCursor::CursorOp > opPtr( new DeleteOp( justOneOrig, best ) );
        shared_ptr< MultiCursor > creal( new MultiCursor( ns, pattern, BSONObj(), opPtr, !god ) );

        if( !creal->ok() )
            return nDeleted;

        shared_ptr< Cursor > cPtr = creal;
        auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout, cPtr, ns) );
        cc->setDoingDeletes( true );

        CursorId id = cc->cursorid();

        bool justOne = justOneOrig;
        bool canYield = !god && !creal->matcher()->docMatcher().atomic();

        do {
            // TODO: we can generalize this I believe
            //       
            bool willNeedRecord = creal->matcher()->needRecord() || pattern.isEmpty() || isSimpleIdQuery( pattern );
            if ( ! willNeedRecord ) {
                // TODO: this is a total hack right now
                // check if the index full encompasses query
                
                if ( pattern.nFields() == 1 && 
                     str::equals( pattern.firstElement().fieldName() , creal->indexKeyPattern().firstElement().fieldName() ) )
                    willNeedRecord = true;
            }
            
            if ( canYield && ! cc->yieldSometimes( willNeedRecord ? ClientCursor::WillNeed : ClientCursor::MaybeCovered ) ) {
                cc.release(); // has already been deleted elsewhere
                // TODO should we assert or something?
                break;
            }
            if ( !cc->ok() ) {
                break; // if we yielded, could have hit the end
            }

            // this way we can avoid calling updateLocation() every time (expensive)
            // as well as some other nuances handled
            cc->setDoingDeletes( true );

            DiskLoc rloc = cc->currLoc();
            BSONObj key = cc->currKey();

            // NOTE Calling advance() may change the matcher, so it's important
            // to try to match first.
            bool match = creal->matcher()->matchesCurrent(creal.get());

            if ( ! cc->advance() )
                justOne = true;

            if ( ! match )
                continue;

            assert( !cc->c()->getsetdup(rloc) ); // can't be a dup, we deleted it!

            if ( !justOne ) {
                /* NOTE: this is SLOW.  this is not good, noteLocation() was designed to be called across getMore
                    blocks.  here we might call millions of times which would be bad.
                    */
                cc->c()->noteLocation();
            }

            if ( logop ) {
                BSONElement e;
                if( BSONObj( rloc.rec() ).getObjectID( e ) ) {
                    BSONObjBuilder b;
                    b.append( e );
                    bool replJustOne = true;
                    logOp( "d", ns, b.done(), 0, &replJustOne );
                }
                else {
                    problem() << "deleted object without id, not logging" << endl;
                }
            }

            if ( rs )
                rs->goingToDelete( rloc.obj() /*cc->c->current()*/ );

            theDataFileMgr.deleteRecord(ns, rloc.rec(), rloc);
            nDeleted++;
            if ( justOne ) {
                break;
            }
            cc->c()->checkLocation();
         
            if( !god ) 
                getDur().commitIfNeeded();

            if( debug && god && nDeleted == 100 ) 
                log() << "warning high number of deletes with god=true which could use significant memory" << endl;
        }
        while ( cc->ok() );

        if ( cc.get() && ClientCursor::find( id , false ) == 0 ) {
            // TODO: remove this and the id declaration above if this doesn't trigger
            //       if it does, then i'm very confused (ERH 06/2011)
            error() << "this should be impossible" << endl;
            printStackTrace();
            cc.release();
        }

        return nDeleted;
    }

}
