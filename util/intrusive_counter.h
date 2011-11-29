/**
 * Copyright (c) 2011 10gen Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/noncopyable.hpp>

namespace mongo {

/*
  IntrusiveCounter is a sharable implementation of a reference counter that
  objects can use to be compatible with boost::intrusive_ptr<>.

  Some objects that use IntrusiveCounter are immutable, and only have
  const methods.  This may require their pointers to be declared as
  intrusive_ptr<const ClassName> .  In order to be able to share pointers to
  these immutables, the methods associated with IntrusiveCounter are declared
  as const, and the counter itself is marked as mutable.

  IntrusiveCounter itself is abstract, allowing for multiple implementations.
  For example, IntrusiveCounterUnsigned uses ordinary unsigned integers for
  the reference count, and is good for situations where thread safety is not
  required.  For others, other implementations using atomic integers should
  be used.  For static objects, the implementations of addRef() and release()
  can be overridden to do nothing.
 */
    class IntrusiveCounter :
        boost::noncopyable {
    public:
	virtual ~IntrusiveCounter() {};

	// these are here for the boost intrusive_ptr<> class
	friend inline void intrusive_ptr_add_ref(const IntrusiveCounter *pIC) {
	    pIC->addRef(); };
	friend inline void intrusive_ptr_release(const IntrusiveCounter *pIC) {
	    pIC->release(); };

	virtual void addRef() const = 0;
	virtual void release() const = 0;
    };

    class IntrusiveCounterUnsigned :
        public IntrusiveCounter {
    public:
	// virtuals from IntrusiveCounter
	virtual void addRef() const;
	virtual void release() const;

	IntrusiveCounterUnsigned();

    private:
	mutable unsigned counter;
    };

};

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline IntrusiveCounterUnsigned::IntrusiveCounterUnsigned():
	counter(0) {
    }

};
