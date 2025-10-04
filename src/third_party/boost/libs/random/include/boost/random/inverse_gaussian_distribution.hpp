/* boost random/inverse_gaussian_distribution.hpp header file
 *
 * Copyright Young Geun Kim 2025
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id$
 */

#ifndef BOOST_RANDOM_INVERSE_GAUSSIAN_DISTRIBUTION_HPP
#define BOOST_RANDOM_INVERSE_GAUSSIAN_DISTRIBUTION_HPP

#include <boost/config/no_tr1/cmath.hpp>
#include <istream>
#include <iosfwd>
#include <limits>
#include <boost/assert.hpp>
#include <boost/limits.hpp>
#include <boost/random/detail/config.hpp>
#include <boost/random/detail/operators.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/random/chi_squared_distribution.hpp>

namespace boost {
namespace random {

/**
 * The inverse gaussian distribution is a real-valued distribution with
 * two parameters alpha (mean) and beta (shape). It produced values > 0.
 *
 * It has
 * \f$\displaystyle p(x) = \sqrt{\beta / (2 \pi x^3)} \exp(-\frac{\beta (x - \alpha)^2}{2 \alpha^2 x})$.
 *
 * The algorithm used is from
 *
 * @blockquote
 * "Generating Random Variates Using Transformations with Multiple Roots",
 * Michael, J. R., Schucany, W. R. and Haas, R. W.,
 * The American Statistician,
 * Volume 30, Issue 2, 1976, Pages 88 - 90
 * @endblockquote
 */
template<class RealType = double>
class inverse_gaussian_distribution
{
public:
	typedef RealType result_type;
    typedef RealType input_type;

	class param_type {
	public:
		typedef inverse_gaussian_distribution distribution_type;

		/**
		 * Constructs a @c param_type object from the "alpha" and "beta"
         * parameters.
         *
         * Requires: alpha > 0 && beta > 0
		 */
		explicit param_type(RealType alpha_arg = RealType(1.0),
											 RealType beta_arg = RealType(1.0))
			: _alpha(alpha_arg), _beta(beta_arg)
		{
			BOOST_ASSERT(alpha_arg > 0);
			BOOST_ASSERT(beta_arg > 0);
		}

    	/** Returns the "alpha" parameter of the distribution. */
        RealType alpha() const { return _alpha; }
        /** Returns the "beta" parameter of the distribution. */
        RealType beta() const { return _beta; }

    	/** Writes a @c param_type to a @c std::ostream. */
        BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, param_type, parm)
        { os << parm._alpha << ' ' << parm._beta; return os; }

        /** Reads a @c param_type from a @c std::istream. */
        BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, param_type, parm)
        { is >> parm._alpha >> std::ws >> parm._beta; return is; }

    	/** Returns true if the two sets of parameters are the same. */
        BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(param_type, lhs, rhs)
        { return lhs._alpha == rhs._alpha && lhs._beta == rhs._beta; }

        /** Returns true if the two sets fo parameters are different. */
        BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(param_type)

    	private:
    		RealType _alpha;
    		RealType _beta;
	};

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

	/**
     * Constructs an @c inverse_gaussian_distribution from its "alpha" and "beta" parameters.
     *
     * Requires: alpha > 0, beta > 0
     */
	explicit inverse_gaussian_distribution(RealType alpha_arg = RealType(1.0),
											 									 RealType beta_arg = RealType(1.0))
		: _alpha(alpha_arg), _beta(beta_arg)
	{
		BOOST_ASSERT(alpha_arg > 0);
		BOOST_ASSERT(beta_arg > 0);
		init();
	}

	/** Constructs an @c inverse_gaussian_distribution from its parameters. */
	explicit inverse_gaussian_distribution(const param_type& parm)
		: _alpha(parm.alpha()), _beta(parm.beta())
	{
		init();
	}

	/**
     * Returns a random variate distributed according to the
     * inverse gaussian distribution.
     */
    template<class URNG>
    RealType operator()(URNG& urng) const
    {
#ifndef BOOST_NO_STDC_NAMESPACE
		using std::sqrt;
#endif
		RealType w = _alpha * chi_squared_distribution<RealType>(result_type(1))(urng);
		RealType cand = _alpha + _c * (w - sqrt(w * (result_type(4) * _beta + w)));
		RealType u = uniform_01<RealType>()(urng);
		if (u < _alpha / (_alpha + cand)) {
			return cand;
		}
    return _alpha * _alpha / cand;
    }

    /**
     * Returns a random variate distributed accordint to the beta
     * distribution with parameters specified by @c param.
     */
    template<class URNG>
    RealType operator()(URNG& urng, const param_type& parm) const
    {
        return inverse_gaussian_distribution(parm)(urng);
    }

	/** Returns the "alpha" parameter of the distribution. */
    RealType alpha() const { return _alpha; }
    /** Returns the "beta" parameter of the distribution. */
    RealType beta() const { return _beta; }

	/** Returns the smallest value that the distribution can produce. */
    RealType min BOOST_PREVENT_MACRO_SUBSTITUTION () const
    { return RealType(0.0); }
    /** Returns the largest value that the distribution can produce. */
    RealType max BOOST_PREVENT_MACRO_SUBSTITUTION () const
    { return (std::numeric_limits<RealType>::infinity)(); }

	/** Returns the parameters of the distribution. */
    param_type param() const { return param_type(_alpha, _beta); }
    /** Sets the parameters of the distribution. */
    void param(const param_type& parm)
    {
        _alpha = parm.alpha();
        _beta = parm.beta();
		init();
    }

	/**
     * Effects: Subsequent uses of the distribution do not depend
     * on values produced by any engine prior to invoking reset.
     */
    void reset() { }

	/** Writes an @c inverse_gaussian_distribution to a @c std::ostream. */
    BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, inverse_gaussian_distribution, wd)
    {
        os << wd.param();
        return os;
    }

    /** Reads an @c inverse_gaussian_distribution from a @c std::istream. */
    BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, inverse_gaussian_distribution, wd)
    {
        param_type parm;
        if(is >> parm) {
            wd.param(parm);
        }
        return is;
    }

	/**
     * Returns true if the two instances of @c inverse_gaussian_distribution will
     * return identical sequences of values given equal generators.
     */
    BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(inverse_gaussian_distribution, lhs, rhs)
    { return lhs._alpha == rhs._alpha && lhs._beta == rhs._beta; }

    /**
     * Returns true if the two instances of @c inverse_gaussian_distribution will
     * return different sequences of values given equal generators.
     */
    BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(inverse_gaussian_distribution)

private:
	result_type _alpha;
	result_type _beta;
	// some data precomputed from the parameters
	result_type _c;

	void init()
    {
		_c = _alpha / (result_type(2) * _beta);
    }
};

} // namespace random

using random::inverse_gaussian_distribution;

} // namespace boost

#endif // BOOST_RANDOM_INVERSE_GAUSSIAN_DISTRIBUTION_HPP
