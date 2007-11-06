///////////////////////////////////////////////////////////////////////////////
// state.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_STATE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_STATE_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/shared_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/access.hpp>
#include <boost/xpressive/detail/core/action_state.hpp>
#include <boost/xpressive/detail/core/sub_match_vector.hpp>
#include <boost/xpressive/detail/utility/sequence_stack.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/regex_constants.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// match_context
//
template<typename BidiIter>
struct match_context
{
    match_context()
      : results_ptr_(0)
      , prev_context_(0)
      , next_ptr_(0)
      , traits_(0)
    {
    }

    // pointer to the current match results, passed to actions as a parameter.
    match_results<BidiIter> *results_ptr_;

    // The previous match context, if this match_context corresponds to a nested regex invocation
    match_context<BidiIter> *prev_context_;

    // If this is a nested match, the "next" sub-expression to execute after the nested match
    matchable<BidiIter> const *next_ptr_;

    // A pointer to the current traits object
    void const *traits_;
};

///////////////////////////////////////////////////////////////////////////////
// match_flags
//
struct match_flags
{
    bool match_all_;
    bool match_prev_avail_;
    bool match_bol_;
    bool match_eol_;
    bool match_not_bow_;
    bool match_not_eow_;
    bool match_not_null_;
    bool match_continuous_;
    bool match_partial_;

    explicit match_flags(regex_constants::match_flag_type flags)
      : match_all_(false)
      , match_prev_avail_(0 != (flags & regex_constants::match_prev_avail))
      , match_bol_(match_prev_avail_ || 0 == (flags & regex_constants::match_not_bol))
      , match_eol_(0 == (flags & regex_constants::match_not_eol))
      , match_not_bow_(!match_prev_avail_ && 0 != (flags & regex_constants::match_not_bow))
      , match_not_eow_(0 != (flags & regex_constants::match_not_eow))
      , match_not_null_(0 != (flags & regex_constants::match_not_null))
      , match_continuous_(0 != (flags & regex_constants::match_continuous))
      , match_partial_(0 != (flags & regex_constants::match_partial))
    {
    }
};

///////////////////////////////////////////////////////////////////////////////
// state_type
//
template<typename BidiIter>
struct state_type
  : noncopyable
{
    typedef core_access<BidiIter> access;
    typedef match_context<BidiIter> match_context;
    typedef results_extras<BidiIter> results_extras;
    typedef regex_impl<BidiIter> regex_impl;
    typedef matchable<BidiIter> matchable;
    typedef match_results<BidiIter> match_results;
    typedef sub_match_impl<BidiIter> sub_match_impl;

    BidiIter cur_;
    sub_match_impl *sub_matches_;
    std::size_t mark_count_;
    BidiIter begin_;
    BidiIter end_;

    match_flags flags_;
    bool found_partial_match_;

    match_context context_;
    results_extras *extras_;
    action_state action_state_;

    ///////////////////////////////////////////////////////////////////////////////
    //
    state_type
    (
        BidiIter begin
      , BidiIter end
      , match_results &what
      , regex_impl const &impl
      , regex_constants::match_flag_type flags
    )
      : cur_(begin)
      , sub_matches_(0)
      , mark_count_(0)
      , begin_(begin)
      , end_(end)
      , flags_(flags)
      , found_partial_match_(false)
      , context_() // zero-initializes the fields of context_
      , extras_(&core_access<BidiIter>::get_extras(what))
      , action_state_(core_access<BidiIter>::get_action_state(what))
    {
        // reclaim any cached memory in the match_results struct
        this->extras_->sub_match_stack_.unwind();

        // initialize the context_ struct
        this->init_(impl, what);

        // move all the nested match_results structs into the match_results cache
        this->extras_->results_cache_.reclaim_all(access::get_nested_results(what));
    }

    ///////////////////////////////////////////////////////////////////////////////
    // reset
    //void reset(match_results &what, basic_regex const &rex)
    void reset(match_results &what, regex_impl const &impl)
    {
        this->extras_ = &core_access<BidiIter>::get_extras(what);
        this->context_.prev_context_ = 0;
        this->found_partial_match_ = false;
        this->extras_->sub_match_stack_.unwind();
        this->init_(impl, what);
        this->extras_->results_cache_.reclaim_all(access::get_nested_results(what));
    }

    ///////////////////////////////////////////////////////////////////////////////
    // push_context
    //  called to prepare the state object for a regex match
    match_context push_context(regex_impl const &impl, matchable const &next, match_context &prev)
    {
        // save state
        match_context context = this->context_;

        // create a new nested match_results for this regex
        nested_results<BidiIter> &nested = access::get_nested_results(*context.results_ptr_);
        match_results &what = this->extras_->results_cache_.append_new(nested);

        // (re)initialize the match context
        this->init_(impl, what);

        // create a linked list of match_context structs
        this->context_.prev_context_ = &prev;
        this->context_.next_ptr_ = &next;

        // record the start of the zero-th sub-match
        this->sub_matches_[0].begin_ = this->cur_;

        return context;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // pop_context
    //  called after a nested match failed to restore the context
    void pop_context(regex_impl const &impl, bool success)
    {
        match_context &context = *this->context_.prev_context_;
        if(!success)
        {
            match_results &what = *context.results_ptr_;
            this->uninit_(impl, what);

            // send the match_results struct back to the cache
            nested_results<BidiIter> &nested = access::get_nested_results(what);
            this->extras_->results_cache_.reclaim_last(nested);
        }

        // restore the state
        this->context_ = context;
        match_results &results = *this->context_.results_ptr_;
        this->sub_matches_ = access::get_sub_matches(access::get_sub_match_vector(results));
        this->mark_count_ = results.size();
    }

    ///////////////////////////////////////////////////////////////////////////////
    // swap_context
    void swap_context(match_context &context)
    {
        std::swap(this->context_, context);
        match_results &results = *this->context_.results_ptr_;
        this->sub_matches_ = access::get_sub_matches(access::get_sub_match_vector(results));
        this->mark_count_ = results.size();
    }

    // beginning of buffer
    bool bos() const
    {
        return this->cur_ == this->begin_;
    }

    // end of buffer
    bool eos()
    {
        return this->cur_ == this->end_ && this->found_partial_match();
    }

    // fetch the n-th sub_match
    sub_match_impl &sub_match(int n)
    {
        return this->sub_matches_[n];
    }

    // called when a partial match has succeeded
    void set_partial_match()
    {
        sub_match_impl &sub0 = this->sub_match(0);
        sub0.first = sub0.begin_;
        sub0.second = this->end_;
        sub0.matched = false;
    }

    template<typename Traits>
    Traits const &get_traits() const
    {
        return *static_cast<Traits const *>(this->context_.traits_);
    }

private:

    void init_(regex_impl const &impl, match_results &what)
    {
        regex_id_type const id = impl.xpr_.get();
        std::size_t const total_mark_count = impl.mark_count_ + impl.hidden_mark_count_ + 1;

        // initialize the context and the sub_match vector
        this->context_.results_ptr_ = &what;
        this->context_.traits_ = impl.traits_.get();
        this->mark_count_ = impl.mark_count_ + 1;
        this->sub_matches_ = this->extras_->sub_match_stack_.push_sequence(total_mark_count);
        this->sub_matches_ += impl.hidden_mark_count_;

        // initialize the match_results struct
        access::init_match_results(what, id, this->sub_matches_, this->mark_count_);
    }

    void uninit_(regex_impl const &impl, match_results &)
    {
        extras_->sub_match_stack_.unwind_to(this->sub_matches_ - impl.hidden_mark_count_);
    }

    bool found_partial_match()
    {
        this->found_partial_match_ = true;
        return true;
    }
};

///////////////////////////////////////////////////////////////////////////////
// memento
//
template<typename BidiIter>
struct memento
{
    sub_match_impl<BidiIter> *old_sub_matches_;
    std::size_t nested_results_count_;
};

///////////////////////////////////////////////////////////////////////////////
// save_sub_matches
//
template<typename BidiIter>
inline memento<BidiIter> save_sub_matches(state_type<BidiIter> &state)
{
    memento<BidiIter> mem =
    {
        state.extras_->sub_match_stack_.push_sequence(state.mark_count_, no_fill)
      , state.context_.results_ptr_->nested_results().size()
    };
    std::copy(state.sub_matches_, state.sub_matches_ + state.mark_count_, mem.old_sub_matches_);
    return mem;
}

///////////////////////////////////////////////////////////////////////////////
// restore_sub_matches
//
template<typename BidiIter>
inline void restore_sub_matches(memento<BidiIter> const &mem, state_type<BidiIter> &state)
{
    typedef core_access<BidiIter> access;
    nested_results<BidiIter> &nested = access::get_nested_results(*state.context_.results_ptr_);
    std::size_t count = state.context_.results_ptr_->nested_results().size() - mem.nested_results_count_;
    state.extras_->results_cache_.reclaim_last_n(nested, count);
    std::copy(mem.old_sub_matches_, mem.old_sub_matches_ + state.mark_count_, state.sub_matches_);
    state.extras_->sub_match_stack_.unwind_to(mem.old_sub_matches_);
}

///////////////////////////////////////////////////////////////////////////////
// reclaim_sub_matches
//
template<typename BidiIter>
inline void reclaim_sub_matches(memento<BidiIter> const &mem, state_type<BidiIter> &state)
{
    std::size_t count = state.context_.results_ptr_->nested_results().size() - mem.nested_results_count_;
    if(count == 0)
    {
        state.extras_->sub_match_stack_.unwind_to(mem.old_sub_matches_);
    }
    // else we have we must orphan this block of backrefs because we are using the stack
    // space above it.
}

///////////////////////////////////////////////////////////////////////////////
// traits_cast
//
template<typename Traits, typename BidiIter>
inline Traits const &traits_cast(state_type<BidiIter> const &state)
{
    return state.template get_traits<Traits>();
}

}}} // namespace boost::xpressive::detail

#endif
