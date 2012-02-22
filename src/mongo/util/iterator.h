/**
 * Copyright (c) 2012 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "pch.h"
#include <vector>
#include "util/intrusive_counter.h"

namespace mongo {

    /**
       Java style iterator.

       Useful for wrapping C++ iterators so that external clients don't need
       to be given access to the container in order to be able to use
       container.end(); or, precludes the need to give clients both an iterator
       and container.end()'s value so they can check for it.

       @param T the type of the objects this will iterate over
     */
    template<class T>
    class Iterator :
        public IntrusiveCounterUnsigned {
    public:
        virtual ~Iterator() {};

        /**
           Check to see if the iterator has anything more to fetch.

           @returns true if there's something to fetch, false otherwise
         */
        virtual bool hasNext() = 0;

        /**
           Get the next item from the iterator.

           It is an error to call this if hasNext() has not returned true.

           @returns the next item
           @throws an exception if there is no next element
         */
        virtual T next() = 0;
    };

    /**
       Implementation of Iterator<T> for boost::vector<T>.

       Depending on circumstances, it might be wise to create a derived class
       of those which also records things such as smart pointers required to
       keep the vector<>'s container alive.

       @param T the vector's template type
    */
    template<class T>
    class IteratorVector :
        public Iterator<T> {
    public:
        // virtuals from class Iterator<T>
        virtual bool hasNext();
        virtual T next();

        /**
           Constructor.

           @param vector the vector to iterate over
        */
        IteratorVector(const vector<T> &vector);

    private:
        typename vector<T>::const_iterator it;
        typename vector<T>::const_iterator end;
    };

    /**
       Implementation of IteratorVector that can be used to keep alive
       a container's owner if it can be held by an intrusive_ptr<>.

       @param T the vector's template type
       @param O the vector's owning object's type
     */
    template<class T, class O>
    class IteratorVectorIntrusive :
        public IteratorVector<T> {
    public:

        /**
           Constructor.

           @param vector the vector to iterate over
           @param the object that owns the vector
        */
        IteratorVectorIntrusive(
            const vector<T> &vector,
            const intrusive_ptr<const O> &pOwner);

    private:
        intrusive_ptr<const O> pOwner;
    };

}


/* ========================= PRIVATE IMPLEMENTATION ======================== */

namespace mongo {

    template<class T>
    inline IteratorVector<T>::IteratorVector(const vector<T> &vector):
        it(vector.begin()),
        end(vector.end()) {
    }

    template<class T>
    inline bool IteratorVector<T>::hasNext() {
        return it < end;
    }

    template<class T>
    inline T IteratorVector<T>::next() {
        T rv = *it;
        ++it;
        return rv;
    }

    template<class T, class O>
    inline IteratorVectorIntrusive<T, O>::IteratorVectorIntrusive(
        const vector<T> &vector, const intrusive_ptr<const O> &pO):
        IteratorVector<T>(vector),
        pOwner(pO) {
    }

}
