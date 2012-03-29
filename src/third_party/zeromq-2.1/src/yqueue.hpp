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

#ifndef __ZMQ_YQUEUE_HPP_INCLUDED__
#define __ZMQ_YQUEUE_HPP_INCLUDED__

#include <stdlib.h>
#include <stddef.h>

#include "err.hpp"
#include "atomic_ptr.hpp"

namespace zmq
{

    //  yqueue is an efficient queue implementation. The main goal is
    //  to minimise number of allocations/deallocations needed. Thus yqueue
    //  allocates/deallocates elements in batches of N.
    //
    //  yqueue allows one thread to use push/back function and another one 
    //  to use pop/front functions. However, user must ensure that there's no
    //  pop on the empty queue and that both threads don't access the same
    //  element in unsynchronised manner.
    //
    //  T is the type of the object in the queue.
    //  N is granularity of the queue (how many pushes have to be done till
    //  actual memory allocation is required).

    template <typename T, int N> class yqueue_t
    {
    public:

        //  Create the queue.
        inline yqueue_t ()
        {
             begin_chunk = (chunk_t*) malloc (sizeof (chunk_t));
             alloc_assert (begin_chunk);
             begin_pos = 0;
             back_chunk = NULL;
             back_pos = 0;
             end_chunk = begin_chunk;
             end_pos = 0;
        }

        //  Destroy the queue.
        inline ~yqueue_t ()
        {
            while (true) {
                if (begin_chunk == end_chunk) {
                    free (begin_chunk);
                    break;
                } 
                chunk_t *o = begin_chunk;
                begin_chunk = begin_chunk->next;
                free (o);
            }

            chunk_t *sc = spare_chunk.xchg (NULL);
            if (sc)
                free (sc);
        }

        //  Returns reference to the front element of the queue.
        //  If the queue is empty, behaviour is undefined.
        inline T &front ()
        {
             return begin_chunk->values [begin_pos];
        }

        //  Returns reference to the back element of the queue.
        //  If the queue is empty, behaviour is undefined.
        inline T &back ()
        {
            return back_chunk->values [back_pos];
        }

        //  Adds an element to the back end of the queue.
        inline void push ()
        {
            back_chunk = end_chunk;
            back_pos = end_pos;

            if (++end_pos != N)
                return;

            chunk_t *sc = spare_chunk.xchg (NULL);
            if (sc) {
                end_chunk->next = sc;
                sc->prev = end_chunk;
            } else {
                end_chunk->next = (chunk_t*) malloc (sizeof (chunk_t));
                alloc_assert (end_chunk->next);
                end_chunk->next->prev = end_chunk;
            }
            end_chunk = end_chunk->next;
            end_pos = 0;
        }

        //  Removes element from the back end of the queue. In other words
        //  it rollbacks last push to the queue. Take care: Caller is
        //  responsible for destroying the object being unpushed.
        //  The caller must also guarantee that the queue isn't empty when
        //  unpush is called. It cannot be done automatically as the read
        //  side of the queue can be managed by different, completely
        //  unsynchronised thread.
        inline void unpush ()
        {
            //  First, move 'back' one position backwards.
            if (back_pos)
                --back_pos;
            else {
                back_pos = N - 1;
                back_chunk = back_chunk->prev;
            }

            //  Now, move 'end' position backwards. Note that obsolete end chunk
            //  is not used as a spare chunk. The analysis shows that doing so
            //  would require free and atomic operation per chunk deallocated
            //  instead of a simple free.
            if (end_pos)
                --end_pos;
            else {
                end_pos = N - 1;
                end_chunk = end_chunk->prev;
                free (end_chunk->next);
                end_chunk->next = NULL;
            }
        }

        //  Removes an element from the front end of the queue.
        inline void pop ()
        {
            if (++ begin_pos == N) {
                chunk_t *o = begin_chunk;
                begin_chunk = begin_chunk->next;
                begin_chunk->prev = NULL;
                begin_pos = 0;

                //  'o' has been more recently used than spare_chunk,
                //  so for cache reasons we'll get rid of the spare and
                //  use 'o' as the spare.
                chunk_t *cs = spare_chunk.xchg (o);
                if (cs)
                    free (cs);
            }
        }

    private:

        //  Individual memory chunk to hold N elements.
        struct chunk_t
        {
             T values [N];
             chunk_t *prev;
             chunk_t *next;
        };

        //  Back position may point to invalid memory if the queue is empty,
        //  while begin & end positions are always valid. Begin position is
        //  accessed exclusively be queue reader (front/pop), while back and
        //  end positions are accessed exclusively by queue writer (back/push).
        chunk_t *begin_chunk;
        int begin_pos;
        chunk_t *back_chunk;
        int back_pos;
        chunk_t *end_chunk;
        int end_pos;

        //  People are likely to produce and consume at similar rates.  In
        //  this scenario holding onto the most recently freed chunk saves
        //  us from having to call malloc/free.
        atomic_ptr_t<chunk_t> spare_chunk;

        //  Disable copying of yqueue.
        yqueue_t (const yqueue_t&);
        const yqueue_t &operator = (const yqueue_t&);
    };

}

#endif
