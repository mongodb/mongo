//=======================================================================
// Copyright 2002 Indiana University.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#ifndef BOOST_BITSET_ADAPTOR_HPP
#define BOOST_BITSET_ADAPTOR_HPP

    template <class T, class Derived>
    struct bitset_adaptor {
      Derived& derived() { return static_cast<Derived&>(*this); }
      const Derived& derived() const { 
        return static_cast<const Derived&>(*this); 
      }
    };

    template <class T, class D, class V>
    bool set_contains(const bitset_adaptor<T,D>& s, const V& x) {
      return s.derived().test(x);
    }
    
    template <class T, class D>
    bool set_equal(const bitset_adaptor<T,D>& x,
                   const bitset_adaptor<T,D>& y) {
      return x.derived() == y.derived();
    }

    template <class T, class D>
    int set_lex_order(const bitset_adaptor<T,D>& x,
                      const bitset_adaptor<T,D>& y) {
      return compare_3way(x.derived(), y.derived());
    }

    template <class T, class D>
    void set_clear(bitset_adaptor<T,D>& x) {
      x.derived().reset();
    }

    template <class T, class D>
    bool set_empty(const bitset_adaptor<T,D>& x) {
      return x.derived().none();
    }

    template <class T, class D, class V>
    void set_insert(bitset_adaptor<T,D>& x, const V& a) {
      x.derived().set(a);
    }

    template <class T, class D, class V>
    void set_remove(bitset_adaptor<T,D>& x, const V& a) {
      x.derived().set(a, false);
    }
    
    template <class T, class D>    
    void set_intersect(const bitset_adaptor<T,D>& x,
                       const bitset_adaptor<T,D>& y,
                       bitset_adaptor<T,D>& z)
    {
      z.derived() = x.derived() & y.derived();
    }

    template <class T, class D>    
    void set_union(const bitset_adaptor<T,D>& x,
                   const bitset_adaptor<T,D>& y,
                   bitset_adaptor<T,D>& z)
    {
      z.derived() = x.derived() | y.derived();
    }

    template <class T, class D>    
    void set_difference(const bitset_adaptor<T,D>& x,
                        const bitset_adaptor<T,D>& y,
                        bitset_adaptor<T,D>& z)
    {
      z.derived() = x.derived() - y.derived();
    }

    template <class T, class D>    
    void set_compliment(const bitset_adaptor<T,D>& x,
                        bitset_adaptor<T,D>& z)
    {
      z.derived() = x.derived();
      z.derived().flip();
    }
    
#endif // BOOST_BITSET_ADAPTOR_HPP
