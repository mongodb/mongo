//  (C) Copyright Marshall Clow 2018
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <iterator> // for std::distance
#include <cassert>  // for assert

#include <boost/algorithm/minmax_element.hpp>
#include <boost/algorithm/cxx11/none_of.hpp>

//	Fuzzing tests for:
//
//		template <class ForwardIterator>
//		std::pair<ForwardIterator,ForwardIterator>
//		first_min_first_max_element(ForwardIterator first, ForwardIterator last);
//
//		template <class ForwardIterator, class BinaryPredicate>
//		std::pair<ForwardIterator,ForwardIterator>
//		first_min_first_max_element(ForwardIterator first, ForwardIterator last,
//	               		BinaryPredicate comp);
//
//	identical signatures for:
//		first_min_last_max_element
//		last_min_first_max_element
//		last_min_last_max_element

bool greater(uint8_t lhs, uint8_t rhs) { return lhs > rhs; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t sz) {
	typedef std::pair<const uint8_t *, const uint8_t *> result_t;
	const uint8_t * const dend = data + sz;
	if (sz == 0) return 0; // we need at least one element
	
	{
//	Find the min and max
	result_t resultff = boost::first_min_first_max_element(data, dend);
	result_t resultfl = boost::first_min_last_max_element (data, dend);
	result_t resultlf = boost::last_min_first_max_element (data, dend);
	result_t resultll = boost::last_min_last_max_element  (data, dend);

//	The iterators have to be in the sequence - and not at the end!
	assert(std::distance(data, resultff.first)  < sz);
	assert(std::distance(data, resultff.second) < sz);
	assert(std::distance(data, resultfl.first)  < sz);
	assert(std::distance(data, resultfl.second) < sz);
	assert(std::distance(data, resultlf.first)  < sz);
	assert(std::distance(data, resultlf.second) < sz);
	assert(std::distance(data, resultll.first)  < sz);
	assert(std::distance(data, resultll.second) < sz);
	
//	the minimum element can't be bigger than the max element

//	Did we find the same min value and max value?
	uint8_t min_value = *resultff.first;
	uint8_t max_value = *resultff.second;
	assert(min_value <= max_value);

//	Each variant should have found the same min/max values
	assert(*resultff.first  == min_value);
	assert(*resultfl.first  == min_value);
	assert(*resultlf.first  == min_value);
	assert(*resultll.first  == min_value);

	assert(*resultff.second == max_value);
	assert(*resultfl.second == max_value);
	assert(*resultlf.second == max_value);
	assert(*resultll.second == max_value);

//	None of the elements in the sequence can be less than the min, nor greater than the max
	for (size_t i = 0; i < sz; ++i) {
		assert(min_value <= data[i]);
		assert(data[i] <= max_value);
		}

//	Make sure we returned the "right" first and last element
	assert(boost::algorithm::none_of_equal(data, resultff.first,     min_value));
	assert(boost::algorithm::none_of_equal(data, resultfl.first,     min_value));
	assert(boost::algorithm::none_of_equal(resultlf.first + 1, dend, min_value));
	assert(boost::algorithm::none_of_equal(resultll.first + 1, dend, min_value));

	assert(boost::algorithm::none_of_equal(data, resultff.second,     max_value));
	assert(boost::algorithm::none_of_equal(resultfl.second + 1, dend, max_value));
	assert(boost::algorithm::none_of_equal(data, resultlf.second,     max_value));
	assert(boost::algorithm::none_of_equal(resultll.second + 1, dend, max_value));
	}
	
	{
//	Find the min and max
	result_t resultff = boost::first_min_first_max_element(data, dend, greater);
	result_t resultfl = boost::first_min_last_max_element (data, dend, greater);
	result_t resultlf = boost::last_min_first_max_element (data, dend, greater);
	result_t resultll = boost::last_min_last_max_element  (data, dend, greater);

//	The iterators have to be in the sequence - and not at the end!
	assert(std::distance(data, resultff.first)  < sz);
	assert(std::distance(data, resultff.second) < sz);
	assert(std::distance(data, resultfl.first)  < sz);
	assert(std::distance(data, resultfl.second) < sz);
	assert(std::distance(data, resultlf.first)  < sz);
	assert(std::distance(data, resultlf.second) < sz);
	assert(std::distance(data, resultll.first)  < sz);
	assert(std::distance(data, resultll.second) < sz);

//	the minimum element can't be bigger than the max element
	uint8_t min_value = *resultff.first;
	uint8_t max_value = *resultff.second;
	
	assert (!greater(max_value, min_value));

//	Each variant should have found the same min/max values
	assert(*resultff.first  == min_value);
	assert(*resultfl.first  == min_value);
	assert(*resultlf.first  == min_value);
	assert(*resultll.first  == min_value);

	assert(*resultff.second == max_value);
	assert(*resultfl.second == max_value);
	assert(*resultlf.second == max_value);
	assert(*resultll.second == max_value);

//	None of the elements in the sequence can be less than the min, nor greater than the max
	for (size_t i = 0; i < sz; ++i) {
		assert(!greater(data[i], min_value));
		assert(!greater(max_value, data[i]));
		}

//	We returned the first min element, and the first max element
	assert(boost::algorithm::none_of_equal(data, resultff.first,     min_value));
	assert(boost::algorithm::none_of_equal(data, resultfl.first,     min_value));
	assert(boost::algorithm::none_of_equal(resultlf.first + 1, dend, min_value));
	assert(boost::algorithm::none_of_equal(resultll.first + 1, dend, min_value));

	assert(boost::algorithm::none_of_equal(data, resultff.second,     max_value));
	assert(boost::algorithm::none_of_equal(resultfl.second + 1, dend, max_value));
	assert(boost::algorithm::none_of_equal(data, resultlf.second,     max_value));
	assert(boost::algorithm::none_of_equal(resultll.second + 1, dend, max_value));
	}

  return 0;
}
