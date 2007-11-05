///////////////////////////////////////////////////////////////////////////////
// sequence_stack.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_SEQUENCE_STACK_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_SEQUENCE_STACK_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning(push)
# pragma warning(disable : 4127) // conditional expression constant
#endif

#include <algorithm>
#include <functional>

namespace boost { namespace xpressive { namespace detail
{

//////////////////////////////////////////////////////////////////////////
// sequence_stack
//
//   For storing a stack of sequences of type T, where each sequence
//   is guaranteed to be stored in contiguous memory.
template<typename T>
struct sequence_stack
{
private:

    struct chunk
    {
        chunk(std::size_t size, std::size_t count, chunk *back, chunk *next)
          : begin_(new T[ size ])
          , curr_(begin_ + count)
          , end_(begin_ + size)
          , back_(back)
          , next_(next)
        {
            if(this->back_)
                this->back_->next_ = this;
            if(this->next_)
                this->next_->back_ = this;
        }

        ~chunk()
        {
            delete[] this->begin_;
        }

        std::size_t size() const
        {
            return static_cast<std::size_t>(this->end_ - this->begin_);
        }

        T *const begin_, *curr_, *const end_;
        chunk *back_, *next_;

    private:
        chunk &operator =(chunk const &);
    };

    chunk *current_chunk_;

    // Cache these for faster access
    T *begin_;
    T *curr_;
    T *end_;

    T *grow_(std::size_t count)
    {
        if(this->current_chunk_)
        {
            // write the cached value of current into the node.
            // OK to do this even if later statements throw.
            this->current_chunk_->curr_ = this->curr_;

            // Do we have a node with enough available memory already?
            if(this->current_chunk_->next_ && count <= this->current_chunk_->next_->size())
            {
                this->current_chunk_ = this->current_chunk_->next_;
                this->curr_ = this->current_chunk_->curr_ = this->current_chunk_->begin_ + count;
                this->end_ = this->current_chunk_->end_;
                this->begin_ = this->current_chunk_->begin_;
                std::fill_n(this->begin_, count, T());
                return this->begin_;
            }

            // grow exponentially
            std::size_t new_size = (std::max)(count, static_cast<std::size_t>(this->current_chunk_->size() * 1.5));

            // Create a new node and insert it into the list
            this->current_chunk_ = new chunk(new_size, count, this->current_chunk_, this->current_chunk_->next_);
        }
        else
        {
            // first chunk is 256
            std::size_t new_size = (std::max)(count, static_cast<std::size_t>(256U));

            // Create a new node and insert it into the list
            this->current_chunk_ = new chunk(new_size, count, 0, 0);
        }

        this->begin_ = this->current_chunk_->begin_;
        this->curr_ = this->current_chunk_->curr_;
        this->end_ = this->current_chunk_->end_;
        return this->begin_;
    }

    void unwind_chunk_()
    {
        // write the cached value of curr_ into current_chunk_
        this->current_chunk_->curr_ = this->begin_;
        // make the previous chunk the current
        this->current_chunk_ = this->current_chunk_->back_;

        // update the cache
        this->begin_ = this->current_chunk_->begin_;
        this->curr_ = this->current_chunk_->curr_;
        this->end_ = this->current_chunk_->end_;
    }

    bool in_current_chunk(T *ptr) const
    {
        return !std::less<void*>()(ptr, this->begin_) && std::less<void*>()(ptr, this->end_);
    }

public:
    sequence_stack()
      : current_chunk_(0)
      , begin_(0)
      , curr_(0)
      , end_(0)
    {
    }

    ~sequence_stack()
    {
        this->clear();
    }

    // walk to the front of the linked list
    void unwind()
    {
        if(this->current_chunk_)
        {
            while(this->current_chunk_->back_)
            {
                this->current_chunk_->curr_ = this->current_chunk_->begin_;
                this->current_chunk_ = this->current_chunk_->back_;
            }

            this->begin_ = this->curr_ = this->current_chunk_->curr_ = this->current_chunk_->begin_;
            this->end_ = this->current_chunk_->end_;
        }
    }

    void clear()
    {
        // walk to the front of the list
        this->unwind();

        // delete the list
        for(chunk *next; this->current_chunk_; this->current_chunk_ = next)
        {
            next = this->current_chunk_->next_;
            delete this->current_chunk_;
        }

        this->begin_ = this->curr_ = this->end_ = 0;
    }

    template<bool Fill>
    T *push_sequence(std::size_t count, mpl::bool_<Fill>)
    {
        // This is the ptr to return
        T *ptr = this->curr_;

        // Advance the high-water mark
        this->curr_ += count;

        // Check to see if we have overflowed this buffer
        if(std::less<void*>()(this->end_, this->curr_))
        {
            // oops, back this out.
            this->curr_ = ptr;

            // allocate a new block and return a ptr to the new memory
            return this->grow_(count);
        }

        if(Fill)
        {
            std::fill_n(ptr, count, T());
        }

        return ptr;
    }

    T *push_sequence(std::size_t count)
    {
        return this->push_sequence(count, mpl::true_());
    }

    void unwind_to(T *ptr)
    {
        while(!this->in_current_chunk(ptr))
        {
            // completely unwind the current chunk, move to the previous chunk
            this->unwind_chunk_();
        }
        this->current_chunk_->curr_ = this->curr_ = ptr;
    }

    // shrink-to-fit: remove any unused nodes in the chain
    void conserve()
    {
        if(this->current_chunk_)
        {
            for(chunk *next; this->current_chunk_->next_; this->current_chunk_->next_ = next)
            {
                next = this->current_chunk_->next_->next_;
                delete this->current_chunk_->next_;
            }
        }
    }
};

typedef mpl::false_ no_fill_t;
no_fill_t const no_fill = {};

}}} // namespace boost::xpressive::detail

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

#endif
