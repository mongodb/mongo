/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ZMQ_ARRAY_INCLUDED__
#define __ZMQ_ARRAY_INCLUDED__

#include <vector>
#include <algorithm>

namespace zmq
{

    //  Base class for objects stored in the array. Note that each object can
    //  be stored in at most one array.

    class array_item_t
    {
    public:

        inline array_item_t () :
            array_index (-1)
        {
        }

        //  The destructor doesn't have to be virtual. It is mad virtual
        //  just to keep ICC and code checking tools from complaining.
        inline virtual ~array_item_t ()
        {
        }

        inline void set_array_index (int index_)
        {
            array_index = index_;
        }

        inline int get_array_index ()
        {
            return array_index;
        }

    private:

        int array_index;

        array_item_t (const array_item_t&);
        const array_item_t &operator = (const array_item_t&);
    };

    //  Fast array implementation with O(1) access to item, insertion and
    //  removal. Array stores pointers rather than objects. The objects have
    //  to be derived from array_item_t class.

    template <typename T> class array_t
    {
    public:

        typedef typename std::vector <T*>::size_type size_type;

        inline array_t ()
        {
        }

        inline ~array_t ()
        {
        }

        inline size_type size ()
        {
            return items.size ();
        }

        inline bool empty ()
        {
            return items.empty ();
        }

        inline T *&operator [] (size_type index_)
        {
            return items [index_];
        }

        inline void push_back (T *item_)
        {
            if (item_)
                item_->set_array_index (items.size ());
            items.push_back (item_);
        }

        inline void erase (T *item_) {
            erase (item_->get_array_index ());
        }

        inline void erase (size_type index_) {
            if (items.back ())
                items.back ()->set_array_index (index_);
            items [index_] = items.back ();
            items.pop_back ();
        }

        inline void swap (size_type index1_, size_type index2_)
        {
            if (items [index1_])
                items [index1_]->set_array_index (index2_);
            if (items [index2_])
                items [index2_]->set_array_index (index1_);
            std::swap (items [index1_], items [index2_]);
        }

        inline void clear ()
        {
            items.clear ();
        }

        inline size_type index (T *item_)
        {
            return (size_type) item_->get_array_index ();
        }

    private:

        typedef std::vector <T*> items_t;
        items_t items;

        array_t (const array_t&);
        const array_t &operator = (const array_t&);
    };

}

#endif
