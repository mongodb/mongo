
/*
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

#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/base/disallow_copying.h"

namespace mongo {

    /* _globalKill: we are shutting down
       otherwise kill attribute set on specified CurOp
       this class does not handle races between interruptJs and the checkForInterrupt functions - those must be
       handled by the client of this class
    */
    class KillCurrentOp {
        MONGO_DISALLOW_COPYING(KillCurrentOp);
    public:
        KillCurrentOp() : _globalKill(false) {}
        void killAll();
        /**
         * @param i opid of operation to kill
         * @return if operation was found 
         **/
        bool kill(AtomicUInt i);
           
        /** 
         * blocks until kill is acknowledged by the killee.
         *
         * Note: Does not wait for nested ops, only the top level op. 
         */
        void blockingKill(AtomicUInt opId);

        /** @return true if global interrupt and should terminate the operation */
        bool globalInterruptCheck() const { return _globalKill; }

        /**
         * @param heedMutex if true and have a write lock, won't kill op since it might be unsafe
         */
        void checkForInterrupt( bool heedMutex = true );

        /** @return "" if not interrupted.  otherwise, you should stop. */
        const char *checkForInterruptNoAssert();

        /** set all flags for all the threads waiting for the current thread's operation to
         *  end; part of internal synchronous kill mechanism
        **/
        void notifyAllWaiters();

        /** Reset the object to its initial state.  Only for testing. */
        void reset();

    private:
        void interruptJs( AtomicUInt *op );
        volatile bool _globalKill;
        boost::condition _condvar;
        boost::mutex _mtx;

        /** 
         * @param i opid of operation to kill
         * @param pNotifyFlag optional bool to be set to true when kill actually happens
         * @return if operation was found 
         **/
        bool _killImpl_inclientlock(AtomicUInt i, bool* pNotifyFlag = NULL);
    };

    extern KillCurrentOp killCurrentOp;
}
