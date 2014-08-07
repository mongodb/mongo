/* boost random/normal_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Copyright Steven Watanabe 2010-2011
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: normal_distribution.hpp 71018 2011-04-05 21:27:52Z steven_watanabe $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_NORMAL_DISTRIBUTION_HPP
#define BOOST_RANDOM_NORMAL_DISTRIBUTION_HPP

#include <boost/config/no_tr1/cmath.hpp>
#include <istream>
#include <iosfwd>
#include <boost/assert.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/detail/config.hpp>
#include <boost/random/detail/operators.hpp>
#include <boost/random/uniform_01.hpp>

namespace boost {
namespace random {

// deterministic Box-Muller method, uses trigonometric functions

/**
 * Instantiations of class template normal_distribution model a
 * \random_distribution. Such a distribution produces random numbers
 * @c x distributed with probability density function
 * \f$\displaystyle p(x) =
 *   \frac{1}{\sqrt{2\pi\sigma}} e^{-\frac{(x-\mu)^2}{2\sigma^2}}
 * \f$,
 * where mean and sigma are the parameters of the distribution.
 */
template<class RealType = double>
class normal_distribution
{
public:
    typedef RealType input_type;
    typedef RealType result_type;

    class param_type {
    public:
        typedef normal_distribution distribution_type;

        /**
         * Constructs a @c param_type with a given mean and
         * standard deviation.
         *
         * Requires: sigma >= 0
         */
        explicit param_type(RealType mean_arg = RealType(0.0),
                            RealType sigma_arg = RealType(1.0))
          : _mean(mean_arg),
            _sigma(sigma_arg)
        {}

        /** Returns the mean of the distribution. */
        RealType mean() const { return _mean; }

        /** Returns the standand deviation of the distribution. */
        RealType sigma() const { return _sigma; }

        /** Writes a @c param_type to a @c std::ostream. */
        BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, param_type, parm)
        { os << parm._mean << " " << parm._sigma ; return os; }

        /** Reads a @c param_type from a @c std::istream. */
        BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, param_type, parm)
        { is >> parm._mean >> std::ws >> parm._sigma; return is; }

        /** Returns true if the two sets of parameters are the same. */
        BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(param_type, lhs, rhs)
        { return lhs._mean == rhs._mean && lhs._sigma == rhs._sigma; }
        
        /** Returns true if the two sets of parameters are the different. */
        BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(param_type)

    private:
        RealType _mean;
        RealType _sigma;
    };

    /**
     * Constructs a @c normal_distribution object. @c mean and @c sigma are
     * the parameters for the distribution.
     *
     * Requires: sigma >= 0
     */
    explicit normal_distribution(const RealType& mean_arg = RealType(0.0),
                                 const RealType& sigma_arg = RealType(1.0))
      : _mean(mean_arg), _sigma(sigma_arg),
        _r1(0), _r2(0), _cached_rho(0), _valid(false)
    {
        BOOST_ASSERT(_sigma >= RealType(0));
    }

    /**
     * Constructs a @c normal_distribution object from its parameters.
     */
    explicit normal_distribution(const param_type& parm)
      : _mean(parm.mean()), _sigma(parm.sigma()),
        _r1(0), _r2(0), _cached_rho(0), _valid(false)
    {}

    /**  Returns the mean of the distribution. */
    RealType mean() const { return _mean; }
    /** Returns the standard deviation of the distribution. */
    RealType sigma() const { return _sigma; }

    /** Returns the smallest value that the distribution can produce. */
    RealType min BOOST_PREVENT_MACRO_SUBSTITUTION () const
    { return -std::numeric_limits<RealType>::infinity(); }
    /** Returns the largest value that the distribution can produce. */
    RealType max BOOST_PREVENT_MACRO_SUBSTITUTION () const
    { return std::numeric_limits<RealType>::infinity(); }

    /** Returns the parameters of the distribution. */
    param_type param() const { return param_type(_mean, _sigma); }
    /** Sets the parameters of the distribution. */
    void param(const param_type& parm)
    {
        _mean = parm.mean();
        _sigma = parm.sigma();
        _valid = false;
    }

    /**
     * Effects: Subsequent uses of the distribution do not depend
     * on values produced by any engine prior to invoking reset.
     */
    void reset() { _valid = false; }

    /**  Returns a normal variate. */
    template<class Engine>
    result_type operator()(Engine& eng)
    {
        using std::sqrt;
        using std::log;
        using std::sin;
        using std::cos;

        if(!_valid) {
            _r1 = boost::uniform_01<RealType>()(eng);
            _r2 = boost::uniform_01<RealType>()(eng);
            _cached_rho = sqrt(-result_type(2) * log(result_type(1)-_r2));
            _valid = true;
        } else {
            _valid = false;
        }
        // Can we have a boost::mathconst please?
        const result_type pi = result_type(3.14159265358979323846);

        return _cached_rho * (_valid ?
                              cos(result_type(2)*pi*_r1) :
                              sin(result_type(2)*pi*_r1))
            * _sigma + _mean;
    }

    /** Returns a normal variate with parameters specified by @c param. */
    template<class URNG>
    result_type operator()(URNG& urng, const param_type& parm)
    {
        return normal_distribution(parm)(urng);
    }

    /** Writes a @c normal_distribution to a @c std::ostream. */
    BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, normal_distribution, nd)
    {
        os << nd._mean << " " << nd._sigma << " "
           << nd._valid << " " << nd._cached_rho << " " << nd._r1;
        return os;
    }

    /** Reads a @c normal_distribution from a @c std::istream. */
    BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, normal_distribution, nd)
    {
        is >> std::ws >> nd._mean >> std::ws >> nd._sigma
           >> std::ws >> nd._valid >> std::ws >> nd._cached_rho
           >> std::ws >> nd._r1;
        return is;
    }

    /**
     * Returns true if the two instances of @c normal_distribution will
     * return identical sequences of values given equal generators.
     */
    BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(normal_distribution, lhs, rhs)
    {
        return lhs._mean == rhs._mean && lhs._sigma == rhs._sigma
            && lhs._valid == rhs._valid
            && (!lhs._valid || (lhs._r1 == rhs._r1 && lhs._r2 == rhs._r2));
    }

    /**
     * Returns true if the two instances of @c normal_distribution will
     * return different sequences of values given equal generators.
     */
    BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(normal_distribution)

private:
    RealType _mean, _sigma;
    RealType _r1, _r2, _cached_rho;
    bool _valid;

};

} // namespace random

using random::normal_distribution;

} // namespace boost

#endif // BOOST_RANDOM_NORMAL_DISTRIBUTION_HPP
