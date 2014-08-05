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

#pragma once

#include "mongo/db/repl/health.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
namespace repl {

    /**
     * most operations on a ReplSet object should be done while locked. that
     * logic implemented here.
     *
     * Order of locking: lock the replica set, then take a rwlock.
     */
    class RSBase : boost::noncopyable {
    private:
        mongo::mutex m;
        int _locked;
        ThreadLocalValue<bool> _lockedByMe;
    protected:
        RSBase() : m("RSBase"), _locked(0) { }
        ~RSBase() { }

    public:
        class lock {
            RSBase& rsbase;
            std::auto_ptr<scoped_lock> sl;
        public:
            lock(RSBase* b) : rsbase(*b) {
                if( rsbase._lockedByMe.get() )
                    return; // recursive is ok...

                sl.reset( new scoped_lock(rsbase.m) );
                DEV verify(rsbase._locked == 0);
                rsbase._locked++;
                rsbase._lockedByMe.set(true);
            }
            ~lock() {
                if( sl.get() ) {
                    verify( rsbase._lockedByMe.get() );
                    DEV verify(rsbase._locked == 1);
                    rsbase._lockedByMe.set(false);
                    rsbase._locked--;
                }
            }
        };

        /* for asserts */
        bool locked() const { return _locked != 0; }

        /** if true, is locked, and was locked by this thread. note if false, it could be in the
         *  lock or not for another just for asserts & such so we can make the contracts clear on
         *  who locks what when.  we don't use these locks that frequently, so the little bit of
         * overhead is fine.
         */
        bool lockedByMe() { return _lockedByMe.get(); }
    };

} // namespace repl
} // namespace mongo
