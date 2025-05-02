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
//		minmax_element(ForwardIterator first, ForwardIterator last);
//
//		template <class ForwardIterator, class BinaryPredicate>
//		std::pair<ForwardIterator,ForwardIterator>
//		minmax_element(ForwardIterator first, ForwardIterator last,
//	               		BinaryPredicate comp);


bool greater(uint8_t lhs, uint8_t rhs) { return lhs > rhs; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t sz) {
	typedef std::pair<const uint8_t *, const uint8_t *> result_t;
	if (sz == 0) return 0; // we need at least one element
	
	{
//	Find the min and max
	result_t result = boost::minmax_element(data, data + sz);

//	The iterators have to be in the sequence - and not at the end!
	assert(std::distance(data, result.first)  < sz);
	assert(std::distance(data, result.second) < sz);
	
//	the minimum element can't be bigger than the max element
	uint8_t min_value = *result.first;
	uint8_t max_value = *result.second;
	
	assert(min_value <= max_value);

//	None of the elements in the sequence can be less than the min, nor greater than the max
	for (size_t i = 0; i < sz; ++i) {
		assert(min_value <= data[i]);
		assert(data[i] <= max_value);
		}

//	We returned the first min element, and the first max element
	assert(boost::algorithm::none_of_equal(data, result.first,  min_value));
	assert(boost::algorithm::none_of_equal(data, result.second, max_value));
	}
	
	{
//	Find the min and max
	result_t result = boost::minmax_element(data, data + sz, greater);

//	The iterators have to be in the sequence - and not at the end!
	assert(std::distance(data, result.first)  < sz);
	assert(std::distance(data, result.second) < sz);

//	the minimum element can't be bigger than the max element
	uint8_t min_value = *result.first;
	uint8_t max_value = *result.second;
	
	assert (!greater(max_value, min_value));

//	None of the elements in the sequence can be less than the min, nor greater than the max
	for (size_t i = 0; i < sz; ++i) {
		assert(!greater(data[i], min_value));
		assert(!greater(max_value, data[i]));
		}

//	We returned the first min element, and the first max element
	assert(boost::algorithm::none_of_equal(data, result.first,  min_value));
	assert(boost::algorithm::none_of_equal(data, result.second, max_value));
	}

  return 0;
}
