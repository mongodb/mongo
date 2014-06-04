/* boost random/uniform_on_sphere.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Copyright Steven Watanabe 2011
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: uniform_on_sphere.hpp 71018 2011-04-05 21:27:52Z steven_watanabe $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_UNIFORM_ON_SPHERE_HPP
#define BOOST_RANDOM_UNIFORM_ON_SPHERE_HPP

#include <vector>
#include <algorithm>     // std::transform
#include <functional>    // std::bind2nd, std::divides
#include <boost/assert.hpp>
#include <boost/random/detail/config.hpp>
#include <boost/random/detail/operators.hpp>
#include <boost/random/normal_distribution.hpp>

namespace boost {
namespace random {

/**
 * Instantiations of class template uniform_on_sphere model a
 * \random_distribution. Such a distribution produces random
 * numbers uniformly distributed on the unit sphere of arbitrary
 * dimension @c dim. The @c Cont template parameter must be a STL-like
 * container type with begin and end operations returning non-const
 * ForwardIterators of type @c Cont::iterator. 
 */
template<class RealType = double, class Cont = std::vector<RealType> >
class uniform_on_sphere
{
public:
    typedef RealType input_type;
    typedef Cont result_type;

    class param_type
    {
    public:

        typedef uniform_on_sphere distribution_type;

        /**
         * Constructs the parameters of a uniform_on_sphere
         * distribution, given the dimension of the sphere.
         */
        explicit param_type(int dim_arg = 2) : _dim(dim_arg)
        {
            BOOST_ASSERT(_dim >= 0);
        }

        /** Returns the dimension of the sphere. */
        int dim() const { return _dim; }

        /** Writes the parameters to a @c std::ostream. */
        BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, param_type, parm)
        {
            os << parm._dim;
            return os;
        }

        /** Reads the parameters from a @c std::istream. */
        BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, param_type, parm)
        {
            is >> parm._dim;
            return is;
        }

        /** Returns true if the two sets of parameters are equal. */
        BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(param_type, lhs, rhs)
        { return lhs._dim == rhs._dim; }

        /** Returns true if the two sets of parameters are different. */
        BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(param_type)

    private:
        int _dim;
    };

    /**
     * Constructs a @c uniform_on_sphere distribution.
     * @c dim is the dimension of the sphere.
     *
     * Requires: dim >= 0
     */
    explicit uniform_on_sphere(int dim_arg = 2)
      : _container(dim_arg), _dim(dim_arg) { }

    /**
     * Constructs a @c uniform_on_sphere distribution from its parameters.
     */
    explicit uniform_on_sphere(const param_type& parm)
      : _container(parm.dim()), _dim(parm.dim()) { }

    // compiler-generated copy ctor and assignment operator are fine

    /** Returns the dimension of the sphere. */
    int dim() const { return _dim; }

    /** Returns the parameters of the distribution. */
    param_type param() const { return param_type(_dim); }
    /** Sets the parameters of the distribution. */
    void param(const param_type& parm)
    {
        _dim = parm.dim();
        _container.resize(_dim);
    }

    /**
     * Returns the smallest value that the distribution can produce.
     * Note that this is required to approximate the standard library's
     * requirements.  The behavior is defined according to lexicographical
     * comparison so that for a container type of std::vector,
     * dist.min() <= x <= dist.max() where x is any value produced
     * by the distribution.
     */
    result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const
    {
        result_type result(_dim);
        if(_dim != 0) {
            result.front() = RealType(-1.0);
        }
        return result;
    }
    /**
     * Returns the largest value that the distribution can produce.
     * Note that this is required to approximate the standard library's
     * requirements.  The behavior is defined according to lexicographical
     * comparison so that for a container type of std::vector,
     * dist.min() <= x <= dist.max() where x is any value produced
     * by the distribution.
     */
    result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const
    {
        result_type result(_dim);
        if(_dim != 0) {
            result.front() = RealType(1.0);
        }
        return result;
    }

    /**
     * Effects: Subsequent uses of the distribution do not depend
     * on values produced by any engine prior to invoking reset.
     */
    void reset() { _normal.reset(); }

    /**
     * Returns a point uniformly distributed over the surface of
     * a sphere of dimension dim().
     */
    template<class Engine>
    const result_type & operator()(Engine& eng)
    {
        RealType sqsum = 0;
        for(typename Cont::iterator it = _container.begin();
            it != _container.end();
            ++it) {
            RealType val = _normal(eng);
            *it = val;
            sqsum += val * val;
        }
        using std::sqrt;
        // for all i: result[i] /= sqrt(sqsum)
        std::transform(_container.begin(), _container.end(), _container.begin(),
                       std::bind2nd(std::divides<RealType>(), sqrt(sqsum)));
        return _container;
    }

    /**
     * Returns a point uniformly distributed over the surface of
     * a sphere of dimension param.dim().
     */
    template<class Engine>
    result_type operator()(Engine& eng, const param_type& parm) const
    {
        return uniform_on_sphere(parm)(eng);
    }

    /** Writes the distribution to a @c std::ostream. */
    BOOST_RANDOM_DETAIL_OSTREAM_OPERATOR(os, uniform_on_sphere, sd)
    {
        os << sd._dim;
        return os;
    }

    /** Reads the distribution from a @c std::istream. */
    BOOST_RANDOM_DETAIL_ISTREAM_OPERATOR(is, uniform_on_sphere, sd)
    {
        is >> sd._dim;
        sd._container.resize(sd._dim);
        return is;
    }

    /**
     * Returns true if the two distributions will produce identical
     * sequences of values, given equal generators.
     */
    BOOST_RANDOM_DETAIL_EQUALITY_OPERATOR(uniform_on_sphere, lhs, rhs)
    { return lhs._dim == rhs._dim && lhs._normal == rhs._normal; }

    /**
     * Returns true if the two distributions may produce different
     * sequences of values, given equal generators.
     */
    BOOST_RANDOM_DETAIL_INEQUALITY_OPERATOR(uniform_on_sphere)

private:
    normal_distribution<RealType> _normal;
    result_type _container;
    int _dim;
};

} // namespace random

using random::uniform_on_sphere;

} // namespace boost

#endif // BOOST_RANDOM_UNIFORM_ON_SPHERE_HPP
