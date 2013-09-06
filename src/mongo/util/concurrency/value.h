/* @file value.h
   concurrency helpers DiagStr, Guarded
*/

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include "spin_lock.h"

namespace mongo {

    /** declare that a variable that is "guarded" by a mutex.

        The decl documents the rule.  For example "counta and countb are guarded by xyzMutex":

          Guarded<int, xyzMutex> counta;
          Guarded<int, xyzMutex> countb;

        Upon use, specify the scoped_lock object.  This makes it hard for someone 
        later to forget to be in the lock.  Check is made that it is the right lock in _DEBUG
        builds at runtime.
    */
    template <typename T, SimpleMutex& BY>
    class Guarded {
        T _val;
    public:
        T& ref(const SimpleMutex::scoped_lock& lk) {
            dassert( &lk.m() == &BY );
            return _val;
        }
    };

    // todo: rename this to ThreadSafeString or something
    /** there is now one mutex per DiagStr.  If you have hundreds or millions of
        DiagStrs you'll need to do something different.
    */
    class DiagStr {
        mutable SpinLock m;
        string _s;
    public:
        DiagStr(const DiagStr& r) : _s(r.get()) { }
        DiagStr(const string& r) : _s(r) { }
        DiagStr() { }
        bool empty() const { 
            scoped_spinlock lk(m);
            return _s.empty();
        }
        string get() const { 
            scoped_spinlock lk(m);
            return _s;
        }
        void set(const char *s) {
            scoped_spinlock lk(m);
            _s = s;
        }
        void set(const string& s) { 
            scoped_spinlock lk(m);
            _s = s;
        }
        operator string() const { return get(); }
        void operator=(const string& s) { set(s); }
        void operator=(const DiagStr& rhs) { 
            set( rhs.get() );
        }

        // == is not defined.  use get() == ... instead.  done this way so one thinks about if composing multiple operations
        bool operator==(const string& s) const; 
    };

}
