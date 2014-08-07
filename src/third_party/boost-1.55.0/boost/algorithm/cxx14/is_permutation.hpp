/* 
   Copyright (c) Marshall Clow 2013

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/// \file  equal.hpp
/// \brief Determines if one 
/// \author Marshall Clow

#ifndef BOOST_ALGORITHM_IS_PERMUTATION_HPP
#define BOOST_ALGORITHM_IS_PERMUTATION_HPP

#include <algorithm>
#include <functional>	// for std::equal_to

namespace boost { namespace algorithm {

namespace detail {

	template <class T1, class T2>
	struct is_perm_eq : public std::binary_function<T1, T2, bool> {
		bool operator () ( const T1& v1, const T2& v2 ) const { return v1 == v2 ;}
		};
	

	template <class RandomAccessIterator1, class RandomAccessIterator2, class BinaryPredicate>
	bool is_permutation ( RandomAccessIterator1 first1, RandomAccessIterator1 last1, 
				 RandomAccessIterator2 first2, RandomAccessIterator2 last2, BinaryPredicate pred,
				 std::random_access_iterator_tag, std::random_access_iterator_tag )
	{
	//	Random-access iterators let is check the sizes in constant time
		if ( std::distance ( first1, last1 ) != std::distance ( first2, last2 ))
			return false;
	// If we know that the sequences are the same size, the original version is fine
		return std::is_permutation ( first1, last1, first2, pred );
	}


	template<class ForwardIterator1, class ForwardIterator2, class BinaryPredicate>
	bool is_permutation (
				ForwardIterator1 first1, ForwardIterator1 last1,
				ForwardIterator2 first2, ForwardIterator2 last2, 
				BinaryPredicate pred,
				std::forward_iterator_tag, std::forward_iterator_tag )
	{

	//	Look for common prefix
		for (; first1 != last1 && first2 != last2; ++first1, ++first2)
			if (!pred(*first1, *first2))
				goto not_done;
	//	We've reached the end of one of the sequences without a mismatch.
		return first1 == last1 && first2 == last2;
	not_done:

	//	Check and make sure that we have the same # of elements left
		typedef typename std::iterator_traits<ForwardIterator1>::difference_type diff1_t;
		diff1_t len1 = _VSTD::distance(first1, last1);
		typedef typename std::iterator_traits<ForwardIterator2>::difference_type diff2_t;
		diff2_t len2 = _VSTD::distance(first2, last2);
		if (len1 != len2)
			return false;

	//	For each element in [f1, l1) see if there are the 
	//	same number of equal elements in [f2, l2)
		for ( ForwardIterator1 i = first1; i != last1; ++i )
		{
		//	Have we already counted this value?
			ForwardIterator1 j;
			for ( j = first1; j != i; ++j )
				if (pred(*j, *i))
					break;
			if ( j == i )	// didn't find it...
			{
			//	Count number of *i in [f2, l2)
				diff1_t c2 = 0;
				for ( ForwardIterator2 iter2 = first2; iter2 != last2; ++iter2 )
					if (pred(*i, *iter2))
						++c2;
				if (c2 == 0)
					return false;

			//	Count number of *i in [i, l1)
				diff1_t c1 = 0;
				for (_ForwardIterator1 iter1 = i; iter1 != last1; ++iter1 )
					if (pred(*i, *iter1))
						++c1;
				if (c1 != c2)
					return false;
			}
		}
		return true;
	}

}


template<class ForwardIterator1, class ForwardIterator2, class BinaryPredicate>
bool is_permutation (
			ForwardIterator1 first1, ForwardIterator1 last1,
			ForwardIterator2 first2, ForwardIterator2 last2, 
			BinaryPredicate pred )
{
	return boost::algorithm::detail::is_permutation ( 
		first1, last1, first2, last2, pred,
		typename std::iterator_traits<ForwardIterator1>::iterator_category (),
		typename std::iterator_traits<ForwardIterator2>::iterator_category ());
}

template<class ForwardIterator1, class ForwardIterator2>
bool is_permutation ( ForwardIterator1 first1, ForwardIterator1 last1,
					  ForwardIterator2 first2, ForwardIterator2 last2 )
{
    typedef typename iterator_traits<_ForwardIterator1>::value_type value1_t;
    typedef typename iterator_traits<_ForwardIterator2>::value_type value2_t;
    return boost::algorithm::detail::is_permutation ( 
    		first1, last1, first2, last2, 
      		boost::algorithm::detail::is_perm_eq<
				typename std::iterator_traits<InputIterator1>::value_type,
				typename std::iterator_traits<InputIterator2>::value_type> (),
			typename std::iterator_traits<ForwardIterator1>::iterator_category (),
			typename std::iterator_traits<ForwardIterator2>::iterator_category ());
}

//	There are already range-based versions of these.

}} // namespace boost and algorithm

#endif // BOOST_ALGORITHM_IS_PERMUTATION_HPP
