/* boost random/discrete_distribution.hpp header file
 *
 * Copyright Steven Watanabe 2009-2011
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: discrete_distribution.hpp 85813 2013-09-21 20:17:00Z jewillco $
 */

#ifndef BOOST_RANDOM_DISCRETE_DISTRIBUTION_HPP_INCLUDED
#define BOOST_RANDOM_DISCRETE_DISTRIBUTION_HPP_INCLUDED

#include <vector>
#include <limits>
#include <numeric>
#include <utility>
#include <iterator>
#include <boost/assert.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/detail/config.hpp>
#include <boost/random/detail/operators.hpp>
#include <boost/random/detail/vector_io.hpp>

#ifndef BOOST_NO_CXX11_HDR_INITIALIZER_LIST
#include <initializer_list>
#endif

#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>

#include <boost/random/detail/disable_warnings.hpp>

namespace boost {
namespace random {

/**
 * The class @c discrete_distribution models a \random_distribution.
 * It produces integers in the range [0, n) with the probability
 * of producing each value is specified by the parameters of the
 * distribution.
 */
template<class IntType = int, class WeightType = double>
class discrete_distribution {
public:
    typedef WeightType input_type;
    typedef IntType result_type;

    class param_type {
    public:

        typedef discrete_distribution distribution_type;

        /**
         * Constructs a @c param_type object, representing a distribution
         * with \f$p(0) = 1\f$ and \f$p(k|k>0) = 0\f$.
         */
        param_type() : _probabilities(1, static_cast<WeightType>(1)) {}
        /**
         * If @c first == @c last, equivalent to the default constructor.
         * Otherwise, the values of the range represent weights for the
         * possible values of the distribution.
         */
        template<class Iter>
        param_type(Iter first, Iter last) : _probabilities(first, last)
        {
            normalize();
        }
#ifndef BOOST_NO_CXX11_HDR_INITIALIZER_LIST
        /**
         * If wl.size() == 0, equivalent to the default constructor.
         * Otherwise, the values of the @c initializer_list represent
         * weights for the possible values of the distribution.
         */
        param_type(const std::initializer_list<WeightType>& wl)
          : _probabilities(wl)
        {
            normalize();
        }
#endif
        /**
         * If the range is empty, equivalent to the default constructor.
         * Otherwise, the elements of the range represent
         * weights for the possible values of the distribution.
         */
        template<class Range>
        explicit param_type(const Range& range)
          : _probabilities(boost::begin(range), boost::end(range))
        {
            normalize();
        }

        /**
         * If nw is zero, equivalent to the default constructor.
         * Otherwise, the range of the distribution is [0, nw),
         * and the weights are found by  calling fw with values
         * evenly distributed between \f$\mbox{xmin} + \delta/2\f$ and
         * \f$\mbox{xmax} - \delta/2\f$, where
         * \f$\delta = (\mbox{xmax} - \mbox{xmin})/\mbox{nw}\f$.
         */
        template<class Func>
        param_type(std::size_t nw, double xmin, double xmax, Func fw)
        {
            std::size_t n = (nw == 0) ? 1 : nw;
            double delta = (xmax - xmin) / n;
            BOOST_ASSERT(delta > 0);
            for(std::size_t k = 0; k < n; ++k) {
                _probabilities.push_back(fw(xmin + k*delta + delta/2));
            }
            normalize();
        }

        /**
         * Returns a vector containing the probabilities of each possible
         * value of the distribution.
         */
        std::vector<WeightType> probabilities() const
        {
            return _probabilities;
        }

        /** Writes the parameters to a @c std::ostream. */
        BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, param_type, parm)
        {
            detail::print_vector(os, parm._probabilities);
            return os;
        }
        
        /** Reads the parameters from a @c std::istream. */
        BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, param_type, parm)
        {
            std::vector<WeightType> temp;
            detail::read_vector(is, temp);
            if(is) {
                parm._probabilities.swap(temp);
            }
            return is;
        }

        /** Returns true if the two sets of parameters are the same. */
        BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(param_type, lhs, rhs)
        {
            return lhs._probabilities == rhs._probabilities;
        }
        /** Returns true if the two sets of parameters are different. */
        BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(param_type)
    private:
        /// @cond show_private
        friend class discrete_distribution;
        explicit param_type(const discrete_distribution& dist)
          : _probabilities(dist.probabilities())
        {}
        void normalize()
        {
            WeightType sum =
                std::accumulate(_probabilities.begin(), _probabilities.end(),
                                static_cast<WeightType>(0));
            for(typename std::vector<WeightType>::iterator
                    iter = _probabilities.begin(),
                    end = _probabilities.end();
                    iter != end; ++iter)
            {
                *iter /= sum;
            }
        }
        std::vector<WeightType> _probabilities;
        /// @endcond
    };

    /**
     * Creates a new @c discrete_distribution object that has
     * \f$p(0) = 1\f$ and \f$p(i|i>0) = 0\f$.
     */
    discrete_distribution()
    {
        _alias_table.push_back(std::make_pair(static_cast<WeightType>(1),
                                              static_cast<IntType>(0)));
    }
    /**
     * Constructs a discrete_distribution from an iterator range.
     * If @c first == @c last, equivalent to the default constructor.
     * Otherwise, the values of the range represent weights for the
     * possible values of the distribution.
     */
    template<class Iter>
    discrete_distribution(Iter first, Iter last)
    {
        init(first, last);
    }
#ifndef BOOST_NO_CXX11_HDR_INITIALIZER_LIST
    /**
     * Constructs a @c discrete_distribution from a @c std::initializer_list.
     * If the @c initializer_list is empty, equivalent to the default
     * constructor.  Otherwise, the values of the @c initializer_list
     * represent weights for the possible values of the distribution.
     * For example, given the distribution
     *
     * @code
     * discrete_distribution<> dist{1, 4, 5};
     * @endcode
     *
     * The probability of a 0 is 1/10, the probability of a 1 is 2/5,
     * the probability of a 2 is 1/2, and no other values are possible.
     */
    discrete_distribution(std::initializer_list<WeightType> wl)
    {
        init(wl.begin(), wl.end());
    }
#endif
    /**
     * Constructs a discrete_distribution from a Boost.Range range.
     * If the range is empty, equivalent to the default constructor.
     * Otherwise, the values of the range represent weights for the
     * possible values of the distribution.
     */
    template<class Range>
    explicit discrete_distribution(const Range& range)
    {
        init(boost::begin(range), boost::end(range));
    }
    /**
     * Constructs a discrete_distribution that approximates a function.
     * If nw is zero, equivalent to the default constructor.
     * Otherwise, the range of the distribution is [0, nw),
     * and the weights are found by  calling fw with values
     * evenly distributed between \f$\mbox{xmin} + \delta/2\f$ and
     * \f$\mbox{xmax} - \delta/2\f$, where
     * \f$\delta = (\mbox{xmax} - \mbox{xmin})/\mbox{nw}\f$.
     */
    template<class Func>
    discrete_distribution(std::size_t nw, double xmin, double xmax, Func fw)
    {
        std::size_t n = (nw == 0) ? 1 : nw;
        double delta = (xmax - xmin) / n;
        BOOST_ASSERT(delta > 0);
        std::vector<WeightType> weights;
        for(std::size_t k = 0; k < n; ++k) {
            weights.push_back(fw(xmin + k*delta + delta/2));
        }
        init(weights.begin(), weights.end());
    }
    /**
     * Constructs a discrete_distribution from its parameters.
     */
    explicit discrete_distribution(const param_type& parm)
    {
        param(parm);
    }

    /**
     * Returns a value distributed according to the parameters of the
     * discrete_distribution.
     */
    template<class URNG>
    IntType operator()(URNG& urng) const
    {
        BOOST_ASSERT(!_alias_table.empty());
        WeightType test = uniform_01<WeightType>()(urng);
        IntType result = uniform_int<IntType>((min)(), (max)())(urng);
        if(test < _alias_table[result].first) {
            return result;
        } else {
            return(_alias_table[result].second);
        }
    }
    
    /**
     * Returns a value distributed according to the parameters
     * specified by param.
     */
    template<class URNG>
    IntType operator()(URNG& urng, const param_type& parm) const
    {
        while(true) {
            WeightType val = uniform_01<WeightType>()(urng);
            WeightType sum = 0;
            std::size_t result = 0;
            for(typename std::vector<WeightType>::const_iterator
                iter = parm._probabilities.begin(),
                end = parm._probabilities.end();
                iter != end; ++iter, ++result)
            {
                sum += *iter;
                if(sum > val) {
                    return result;
                }
            }
        }
    }
    
    /** Returns the smallest value that the distribution can produce. */
    result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return 0; }
    /** Returns the largest value that the distribution can produce. */
    result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const
    { return static_cast<result_type>(_alias_table.size() - 1); }

    /**
     * Returns a vector containing the probabilities of each
     * value of the distribution.  For example, given
     *
     * @code
     * discrete_distribution<> dist = { 1, 4, 5 };
     * std::vector<double> p = dist.param();
     * @endcode
     *
     * the vector, p will contain {0.1, 0.4, 0.5}.
     */
    std::vector<WeightType> probabilities() const
    {
        std::vector<WeightType> result(_alias_table.size());
        const WeightType mean =
            static_cast<WeightType>(1) / _alias_table.size();
        std::size_t i = 0;
        for(typename alias_table_t::const_iterator
                iter = _alias_table.begin(),
                end = _alias_table.end();
                iter != end; ++iter, ++i)
        {
            WeightType val = iter->first * mean;
            result[i] += val;
            result[iter->second] += mean - val;
        }
        return(result);
    }

    /** Returns the parameters of the distribution. */
    param_type param() const
    {
        return param_type(*this);
    }
    /** Sets the parameters of the distribution. */
    void param(const param_type& parm)
    {
        init(parm._probabilities.begin(), parm._probabilities.end());
    }
    
    /**
     * Effects: Subsequent uses of the distribution do not depend
     * on values produced by any engine prior to invoking reset.
     */
    void reset() {}

    /** Writes a distribution to a @c std::ostream. */
    BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, discrete_distribution, dd)
    {
        os << dd.param();
        return os;
    }

    /** Reads a distribution from a @c std::istream */
    BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, discrete_distribution, dd)
    {
        param_type parm;
        if(is >> parm) {
            dd.param(parm);
        }
        return is;
    }

    /**
     * Returns true if the two distributions will return the
     * same sequence of values, when passed equal generators.
     */
    BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(discrete_distribution, lhs, rhs)
    {
        return lhs._alias_table == rhs._alias_table;
    }
    /**
     * Returns true if the two distributions may return different
     * sequences of values, when passed equal generators.
     */
    BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(discrete_distribution)

private:

    /// @cond show_private

    template<class Iter>
    void init(Iter first, Iter last, std::input_iterator_tag)
    {
        std::vector<WeightType> temp(first, last);
        init(temp.begin(), temp.end());
    }
    template<class Iter>
    void init(Iter first, Iter last, std::forward_iterator_tag)
    {
        std::vector<std::pair<WeightType, IntType> > below_average;
        std::vector<std::pair<WeightType, IntType> > above_average;
        std::size_t size = std::distance(first, last);
        WeightType weight_sum =
            std::accumulate(first, last, static_cast<WeightType>(0));
        WeightType weight_average = weight_sum / size;
        std::size_t i = 0;
        for(; first != last; ++first, ++i) {
            WeightType val = *first / weight_average;
            std::pair<WeightType, IntType> elem(val, static_cast<IntType>(i));
            if(val < static_cast<WeightType>(1)) {
                below_average.push_back(elem);
            } else {
                above_average.push_back(elem);
            }
        }

        _alias_table.resize(size);
        typename alias_table_t::iterator
            b_iter = below_average.begin(),
            b_end = below_average.end(),
            a_iter = above_average.begin(),
            a_end = above_average.end()
            ;
        while(b_iter != b_end && a_iter != a_end) {
            _alias_table[b_iter->second] =
                std::make_pair(b_iter->first, a_iter->second);
            a_iter->first -= (static_cast<WeightType>(1) - b_iter->first);
            if(a_iter->first < static_cast<WeightType>(1)) {
                *b_iter = *a_iter++;
            } else {
                ++b_iter;
            }
        }
        for(; b_iter != b_end; ++b_iter) {
            _alias_table[b_iter->second].first = static_cast<WeightType>(1);
        }
        for(; a_iter != a_end; ++a_iter) {
            _alias_table[a_iter->second].first = static_cast<WeightType>(1);
        }
    }
    template<class Iter>
    void init(Iter first, Iter last)
    {
        if(first == last) {
            _alias_table.clear();
            _alias_table.push_back(std::make_pair(static_cast<WeightType>(1),
                                                  static_cast<IntType>(0)));
        } else {
            typename std::iterator_traits<Iter>::iterator_category category;
            init(first, last, category);
        }
    }
    typedef std::vector<std::pair<WeightType, IntType> > alias_table_t;
    alias_table_t _alias_table;
    /// @endcond
};

}
}

#include <boost/random/detail/enable_warnings.hpp>

#endif
