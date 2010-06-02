// spin_lock.h

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

#ifndef CONCURRENCY_SPINLOCK_HEADER
#define CONCURRENCY_SPINLOCK_HEADER

namespace mongo {

    /**
     * BIG WARNING - COMPILES, BUT NOT READY FOR USE - BIG WARNING
     *
     * The spinlock currently requires late GCC support
     * routines. Support for other platforms will be added soon.
     */
    class SpinLock{
    public:
        SpinLock();
        ~SpinLock();

        void lock();
        void unlock();

    private:
        bool _locked;

        // Non-copyable, non-assignable
        SpinLock(SpinLock&);
        SpinLock& operator=(SpinLock&);
    }; 

}  // namespace mongo

#endif  // CONCURRENCY_SPINLOCK_HEADER
