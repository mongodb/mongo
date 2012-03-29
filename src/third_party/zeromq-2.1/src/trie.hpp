/*
    Copyright (c) 2007-2011 iMatix Corporation
    Copyright (c) 2011-2012 Spotify AB
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

#ifndef __ZMQ_TRIE_HPP_INCLUDED__
#define __ZMQ_TRIE_HPP_INCLUDED__

#include <stddef.h>

#include "stdint.hpp"

namespace zmq
{

    class trie_t
    {
    public:

        trie_t ();
        ~trie_t ();

        void add (unsigned char *prefix_, size_t size_);
        bool rm (unsigned char *prefix_, size_t size_);
        bool check (unsigned char *data_, size_t size_);

    private:
        bool is_redundant () const;

        uint32_t refcnt;
        unsigned char min;
        unsigned short count;
        unsigned short live_nodes;
        union {
            class trie_t *node;
            class trie_t **table;
        } next;

        trie_t (const trie_t&);
        const trie_t &operator = (const trie_t&);
    };

}

#endif

