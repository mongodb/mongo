//
// Copyright (C) 1999 and onwards Google, Inc.
//
//
// In most .h files, we would rather include a declaration of an stl
// rather than including the appropriate stl h file (which brings in
// lots of crap).  For many STL classes this is ok (eg pair), but for
// some it's really annoying.  We define those here, so you can
// just include this file instead of having to deal with the annoyance.
//
// Most of the annoyance, btw, has to do with the default allocator.

#ifndef _STL_DECL_MSVC_H
#define _STL_DECL_MSVC_H

// VC++ namespace / STL issues; make them explicit
#include <wchar.h>
#include <string>
using std::string;

#include <vector>
using std::vector;

#include <functional>
using std::less;

#include <utility>
using std::pair;
using std::make_pair;

#include <set>
using std::set;
using std::multiset;

#include <list>
#define slist list
#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include <deque>
#include <iostream>
using std::ostream;
using std::cout;
using std::endl;

#include <map>
using std::map;
using std::multimap;

#include <queue>
using std::priority_queue;

#include <stack>

// copy_n isn't to be found anywhere in MSVC's STL
template <typename InputIterator, typename Size, typename OutputIterator>
std::pair<InputIterator, OutputIterator>
copy_n(InputIterator in, Size count, OutputIterator out) {
  for ( ; count > 0; --count) {
    *out = *in;
    ++out;
    ++in;
  }
  return std::make_pair(in, out);
}

// Nor are the following selectors
template <typename T>
struct identity {
  inline const T& operator()(const T& t) const { return t; }
};

// Copied from STLport
template <class _Pair>
struct select1st : public std::unary_function<_Pair, typename _Pair::first_type> {
  const typename _Pair::first_type& operator()(const _Pair& __x) const {
    return __x.first;
  }
};

template <class _Pair>
struct select2nd : public std::unary_function<_Pair, typename _Pair::second_type>
{
  const typename _Pair::second_type& operator()(const _Pair& __x) const {
    return __x.second;
  }
};


//#if _MSC_VER >= 1300
#if 0 //_MSC_VER >= 1300

// If you compile on Windows and get a compile-time error because
// some google3 code specifies a 3rd or 4th parameter to one of
// these template classes, then you have to put in some #ifdefs
// and use the NATIVE_HASH_NAMESPACE::hash_(set|map) implementation.
namespace msvchash {
  template <typename Key>
  struct hash;

  template <class Key,
            class HashFcn = hash<Key> >
  class hash_set;

  template <class Key, class Val,
            class HashFcn = hash<Key> >
  class hash_map;

  template <class Key,
            class HashFcn = hash<Key> >
  class hash_multiset;

  template <class Key, class Val,
            class HashFcn = hash<Key> >
  class hash_multimap;
}  // end namespace  msvchash

using msvchash::hash_set;
using msvchash::hash_map;
using msvchash::hash;
using msvchash::hash_multimap;
using msvchash::hash_multiset;

#else
#define hash_map map
#define hash_set set
#endif

using namespace std;

#endif   /* #ifdef _STL_DECL_MSVC_H */
