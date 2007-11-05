// Copyright 2004, 2005 The Trustees of Indiana University.

// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Authors: Jeremiah Willcock
//           Douglas Gregor
//           Andrew Lumsdaine
#ifndef BOOST_GRAPH_ERDOS_RENYI_GENERATOR_HPP
#define BOOST_GRAPH_ERDOS_RENYI_GENERATOR_HPP

#include <cassert>
#include <iterator>
#include <utility>
#include <boost/shared_ptr.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/random/geometric_distribution.hpp>
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/type_traits/is_same.hpp>
#include <cmath>

namespace boost {

  template<typename RandomGenerator, typename Graph>
  class erdos_renyi_iterator
  {
    typedef typename graph_traits<Graph>::directed_category directed_category;
    typedef typename graph_traits<Graph>::vertices_size_type vertices_size_type;
    typedef typename graph_traits<Graph>::edges_size_type edges_size_type;

    BOOST_STATIC_CONSTANT
      (bool,
       is_undirected = (is_base_and_derived<undirected_tag,
                                            directed_category>::value
                        || is_same<undirected_tag, directed_category>::value));

  public:
    typedef std::input_iterator_tag iterator_category;
    typedef std::pair<vertices_size_type, vertices_size_type> value_type;
    typedef const value_type& reference;
    typedef const value_type* pointer;
    typedef void difference_type;

    erdos_renyi_iterator() : gen(), n(0), edges(0), allow_self_loops(false) {}
    erdos_renyi_iterator(RandomGenerator& gen, vertices_size_type n, 
                         double fraction = 0.0, bool allow_self_loops = false)
      : gen(&gen), n(n), edges(edges_size_type(fraction * n * n)),
        allow_self_loops(allow_self_loops)
    { 
      if (is_undirected) edges = edges / 2;
      next(); 
    }

    erdos_renyi_iterator(RandomGenerator& gen, vertices_size_type n, 
                         edges_size_type m, bool allow_self_loops = false)
      : gen(&gen), n(n), edges(m),
        allow_self_loops(allow_self_loops)
    { 
      next(); 
    }

    reference operator*() const { return current; }
    pointer operator->() const { return &current; }
    
    erdos_renyi_iterator& operator++()
    { 
      --edges;
      next();
      return *this;
    }

    erdos_renyi_iterator operator++(int)
    {
      erdos_renyi_iterator temp(*this);
      ++(*this);
      return temp;
    }

    bool operator==(const erdos_renyi_iterator& other) const
    { return edges == other.edges; }

    bool operator!=(const erdos_renyi_iterator& other) const
    { return !(*this == other); }

  private:
    void next()
    {
      uniform_int<vertices_size_type> rand_vertex(0, n-1);
      current.first = rand_vertex(*gen);
      do {
        current.second = rand_vertex(*gen);
      } while (current.first == current.second && !allow_self_loops);
    }

    RandomGenerator* gen;
    vertices_size_type n;
    edges_size_type edges;
    bool allow_self_loops;
    value_type current;
  };

  template<typename RandomGenerator, typename Graph>
  class sorted_erdos_renyi_iterator
  {
    typedef typename graph_traits<Graph>::directed_category directed_category;
    typedef typename graph_traits<Graph>::vertices_size_type vertices_size_type;
    typedef typename graph_traits<Graph>::edges_size_type edges_size_type;

    BOOST_STATIC_CONSTANT
      (bool,
       is_undirected = (is_base_and_derived<undirected_tag,
                                            directed_category>::value
                        || is_same<undirected_tag, directed_category>::value));

  public:
    typedef std::input_iterator_tag iterator_category;
    typedef std::pair<vertices_size_type, vertices_size_type> value_type;
    typedef const value_type& reference;
    typedef const value_type* pointer;
    typedef void difference_type;

    sorted_erdos_renyi_iterator()
      : gen(), rand_vertex(0.5), n(0), allow_self_loops(false),
    src((std::numeric_limits<vertices_size_type>::max)()), tgt(0), prob(0) {}
    sorted_erdos_renyi_iterator(RandomGenerator& gen, vertices_size_type n, 
                    double prob = 0.0, 
                                bool allow_self_loops = false)
      : gen(),
        // The "1.0 - prob" in the next line is to work around a Boost.Random
        // (and TR1) bug in the specification of geometric_distribution.  It
        // should be replaced by "prob" when the issue is fixed.
        rand_vertex(1.0 - prob),
    n(n), allow_self_loops(allow_self_loops), src(0), tgt(0), prob(prob)
    { 
      this->gen.reset(new uniform_01<RandomGenerator>(gen));

      if (prob == 0.0) {src = (std::numeric_limits<vertices_size_type>::max)(); return;}
      next(); 
    }

    reference operator*() const { return current; }
    pointer operator->() const { return &current; }
    
    sorted_erdos_renyi_iterator& operator++()
    { 
      next();
      return *this;
    }

    sorted_erdos_renyi_iterator operator++(int)
    {
      sorted_erdos_renyi_iterator temp(*this);
      ++(*this);
      return temp;
    }

    bool operator==(const sorted_erdos_renyi_iterator& other) const
    { return src == other.src && tgt == other.tgt; }

    bool operator!=(const sorted_erdos_renyi_iterator& other) const
    { return !(*this == other); }

  private:
    void next()
    {
      using std::sqrt;
      using std::floor;

      // In order to get the edges from the generator in sorted order, one
      // effective (but slow) procedure would be to use a
      // bernoulli_distribution for each legal (src, tgt) pair.  Because of the
      // O(n^2) cost of that, a geometric distribution is used.  The geometric
      // distribution tells how many times the bernoulli_distribution would
      // need to be run until it returns true.  Thus, this distribution can be
      // used to step through the edges which are actually present.  Everything
      // beyond "tgt += increment" is done to effectively convert linear
      // indexing (the partial sums of the geometric distribution output) into
      // graph edges.
      assert (src != (std::numeric_limits<vertices_size_type>::max)());
      vertices_size_type increment = rand_vertex(*gen);
      tgt += increment;
      if (is_undirected) {
    // Update src and tgt based on position of tgt
    // Basically, we want the greatest src_increment such that (in \bbQ):
    // src_increment * (src + allow_self_loops + src_increment - 1/2) <= tgt
    // The result of the LHS of this, evaluated with the computed
    // src_increment, is then subtracted from tgt
    double src_minus_half = (src + allow_self_loops) - 0.5;
    double disc = src_minus_half * src_minus_half + 2 * tgt;
    double src_increment_fp = floor(sqrt(disc) - src_minus_half);
    vertices_size_type src_increment = vertices_size_type(src_increment_fp);
    if (src + src_increment >= n) {
      src = n;
    } else {
      tgt -= (src + allow_self_loops) * src_increment + 
         src_increment * (src_increment - 1) / 2;
      src += src_increment;
    }
      } else {
    // Number of out edge positions possible from each vertex in this graph
    vertices_size_type possible_out_edges = n - (allow_self_loops ? 0 : 1);
    src += (std::min)(n - src, tgt / possible_out_edges);
    tgt %= possible_out_edges;
      }
      // Set end of graph code so (src, tgt) will be the same as for the end
      // sorted_erdos_renyi_iterator
      if (src >= n) {src = (std::numeric_limits<vertices_size_type>::max)(); tgt = 0;}
      // Copy (src, tgt) into current
      current.first = src;
      current.second = tgt;
      // Adjust for (src, src) edge being forbidden
      if (!allow_self_loops && tgt >= src) ++current.second;
    }

    shared_ptr<uniform_01<RandomGenerator> > gen;
    geometric_distribution<vertices_size_type> rand_vertex;
    vertices_size_type n;
    bool allow_self_loops;
    vertices_size_type src, tgt;
    value_type current;
    double prob;
  };

} // end namespace boost

#endif // BOOST_GRAPH_ERDOS_RENYI_GENERATOR_HPP
