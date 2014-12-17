//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_ADAPTIVE_NODE_POOL_IMPL_HPP
#define BOOST_CONTAINER_DETAIL_ADAPTIVE_NODE_POOL_IMPL_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/container_fwd.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/math_functions.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/pool_common.hpp>
#include <boost/assert.hpp>
#include <cstddef>

namespace boost {
namespace container {
namespace container_detail {

template<class size_type>
struct hdr_offset_holder_t
{
   hdr_offset_holder_t(size_type offset = 0)
      : hdr_offset(offset)
   {}
   size_type hdr_offset;
};

template<class VoidPointer, class SizeType>
struct adaptive_pool_types
{
   typedef VoidPointer void_pointer;
   typedef typename bi::make_set_base_hook
      < bi::void_pointer<void_pointer>
      , bi::optimize_size<true>
      , bi::constant_time_size<false>
      , bi::link_mode<bi::normal_link> >::type multiset_hook_t;
   
   typedef hdr_offset_holder_t<SizeType> hdr_offset_holder;

   struct block_info_t
      :  
         public hdr_offset_holder,
         public multiset_hook_t
   {
      typedef typename node_slist<void_pointer>::node_slist_t free_nodes_t;
      //An intrusive list of free node from this block
      free_nodes_t free_nodes;
      friend bool operator <(const block_info_t &l, const block_info_t &r)
      {
//      {  return l.free_nodes.size() < r.free_nodes.size();   }
         //Let's order blocks first by free nodes and then by address
         //so that highest address fully free blocks are deallocated.
         //This improves returning memory to the OS (trimming).
         const bool is_less  = l.free_nodes.size() < r.free_nodes.size();
         const bool is_equal = l.free_nodes.size() == r.free_nodes.size();
         return is_less || (is_equal && (&l < &r));
      }

      friend bool operator ==(const block_info_t &l, const block_info_t &r)
      {  return &l == &r;  }
   };
   typedef typename bi::make_multiset
      <block_info_t, bi::base_hook<multiset_hook_t> >::type  block_multiset_t;
};

template<class size_type>
inline size_type calculate_alignment
   ( size_type overhead_percent, size_type real_node_size
   , size_type hdr_size, size_type hdr_offset_size, size_type payload_per_allocation)
{
   //to-do: handle real_node_size != node_size
   const size_type divisor  = overhead_percent*real_node_size;
   const size_type dividend = hdr_offset_size*100;
   size_type elements_per_subblock = (dividend - 1)/divisor + 1;
   size_type candidate_power_of_2 = 
      upper_power_of_2(elements_per_subblock*real_node_size + hdr_offset_size);
   bool overhead_satisfied = false;
   //Now calculate the wors-case overhead for a subblock
   const size_type max_subblock_overhead  = hdr_size + payload_per_allocation;
   while(!overhead_satisfied){
      elements_per_subblock = (candidate_power_of_2 - max_subblock_overhead)/real_node_size;
      const size_type overhead_size = candidate_power_of_2 - elements_per_subblock*real_node_size;
      if(overhead_size*100/candidate_power_of_2 < overhead_percent){
         overhead_satisfied = true;
      }
      else{
         candidate_power_of_2 <<= 1;
      }
   }
   return candidate_power_of_2;
}

template<class size_type>
inline void calculate_num_subblocks
   (size_type alignment, size_type real_node_size, size_type elements_per_block
   , size_type &num_subblocks, size_type &real_num_node, size_type overhead_percent
   , size_type hdr_size, size_type hdr_offset_size, size_type payload_per_allocation)
{
   size_type elements_per_subblock = (alignment - hdr_offset_size)/real_node_size;
   size_type possible_num_subblock = (elements_per_block - 1)/elements_per_subblock + 1;
   size_type hdr_subblock_elements = (alignment - hdr_size - payload_per_allocation)/real_node_size;
   while(((possible_num_subblock-1)*elements_per_subblock + hdr_subblock_elements) < elements_per_block){
      ++possible_num_subblock;
   }
   elements_per_subblock = (alignment - hdr_offset_size)/real_node_size;
   bool overhead_satisfied = false;
   while(!overhead_satisfied){
      const size_type total_data = (elements_per_subblock*(possible_num_subblock-1) + hdr_subblock_elements)*real_node_size;
      const size_type total_size = alignment*possible_num_subblock;
      if((total_size - total_data)*100/total_size < overhead_percent){
         overhead_satisfied = true;
      }
      else{
         ++possible_num_subblock;
      }
   }
   num_subblocks = possible_num_subblock;
   real_num_node = (possible_num_subblock-1)*elements_per_subblock + hdr_subblock_elements;
}

template<class SegmentManagerBase, bool AlignOnly = false>
class private_adaptive_node_pool_impl
{
   //Non-copyable
   private_adaptive_node_pool_impl();
   private_adaptive_node_pool_impl(const private_adaptive_node_pool_impl &);
   private_adaptive_node_pool_impl &operator=(const private_adaptive_node_pool_impl &);
   typedef private_adaptive_node_pool_impl this_type;

   typedef typename SegmentManagerBase::void_pointer void_pointer;
   static const typename SegmentManagerBase::
      size_type PayloadPerAllocation = SegmentManagerBase::PayloadPerAllocation;
   typedef bool_<AlignOnly>            IsAlignOnly;
   typedef true_                       AlignOnlyTrue;
   typedef false_                      AlignOnlyFalse;

   public:
   typedef typename node_slist<void_pointer>::node_t node_t;
   typedef typename node_slist<void_pointer>::node_slist_t free_nodes_t;
   typedef typename SegmentManagerBase::multiallocation_chain     multiallocation_chain;
   typedef typename SegmentManagerBase::size_type                 size_type;

   private:
   typedef typename adaptive_pool_types<void_pointer, size_type>::block_info_t      block_info_t;
   typedef typename adaptive_pool_types<void_pointer, size_type>::block_multiset_t  block_multiset_t;
   typedef typename block_multiset_t::iterator                                      block_iterator;
   typedef typename adaptive_pool_types<void_pointer, size_type>::hdr_offset_holder hdr_offset_holder;

   static const size_type MaxAlign = alignment_of<node_t>::value;
   static const size_type HdrSize  = ((sizeof(block_info_t)-1)/MaxAlign+1)*MaxAlign;
   static const size_type HdrOffsetSize = ((sizeof(hdr_offset_holder)-1)/MaxAlign+1)*MaxAlign;


   public:
   //!Segment manager typedef
   typedef SegmentManagerBase                 segment_manager_base_type;

   //!Constructor from a segment manager. Never throws
   private_adaptive_node_pool_impl
      ( segment_manager_base_type *segment_mngr_base
      , size_type node_size
      , size_type nodes_per_block
      , size_type max_free_blocks
      , unsigned char overhead_percent
      )
   :  m_max_free_blocks(max_free_blocks)
   ,  m_real_node_size(lcm(node_size, size_type(alignment_of<node_t>::value)))
      //Round the size to a power of two value.
      //This is the total memory size (including payload) that we want to
      //allocate from the general-purpose allocator
   ,  m_real_block_alignment
         (AlignOnly ?
            upper_power_of_2(HdrSize + m_real_node_size*nodes_per_block) :
            calculate_alignment( (size_type)overhead_percent, m_real_node_size
                               , HdrSize, HdrOffsetSize, PayloadPerAllocation))
      //This is the real number of nodes per block
   ,  m_num_subblocks(0)
   ,  m_real_num_node(AlignOnly ? (m_real_block_alignment - PayloadPerAllocation - HdrSize)/m_real_node_size : 0)
      //General purpose allocator
   ,  mp_segment_mngr_base(segment_mngr_base)
   ,  m_block_multiset()
   ,  m_totally_free_blocks(0)
   {
      if(!AlignOnly){
         calculate_num_subblocks
            ( m_real_block_alignment
            , m_real_node_size
            , nodes_per_block
            , m_num_subblocks
            , m_real_num_node
            , (size_type)overhead_percent
            , HdrSize
            , HdrOffsetSize
            , PayloadPerAllocation);
      }
   }

   //!Destructor. Deallocates all allocated blocks. Never throws
   ~private_adaptive_node_pool_impl()
   {  priv_clear();  }

   size_type get_real_num_node() const
   {  return m_real_num_node; }

   //!Returns the segment manager. Never throws
   segment_manager_base_type* get_segment_manager_base()const
   {  return container_detail::to_raw_pointer(mp_segment_mngr_base);  }

   //!Allocates array of count elements. Can throw
   void *allocate_node()
   {
      priv_invariants();
      //If there are no free nodes we allocate a new block
      if (m_block_multiset.empty()){ 
         priv_alloc_block(1);
      }
      //We take the first free node the multiset can't be empty
      return priv_take_first_node();
   }

   //!Deallocates an array pointed by ptr. Never throws
   void deallocate_node(void *pElem)
   {
      multiallocation_chain chain;
      chain.push_front(void_pointer(pElem));
      this->priv_reinsert_nodes_in_block(chain, 1);
      //Update free block count<
      if(m_totally_free_blocks > m_max_free_blocks){
         this->priv_deallocate_free_blocks(m_max_free_blocks);
      }
      priv_invariants();
   }

   //!Allocates n nodes. 
   //!Can throw
   multiallocation_chain allocate_nodes(const size_type n)
   {
      multiallocation_chain chain;
      size_type i = 0;
      try{
         priv_invariants();
         while(i != n){
            //If there are no free nodes we allocate all needed blocks
            if (m_block_multiset.empty()){
               priv_alloc_block(((n - i) - 1)/m_real_num_node + 1);
            }
            free_nodes_t &free_nodes = m_block_multiset.begin()->free_nodes;
            const size_type free_nodes_count_before = free_nodes.size();
            if(free_nodes_count_before == m_real_num_node){
               --m_totally_free_blocks;
            }
            const size_type num_elems = ((n-i) < free_nodes_count_before) ? (n-i) : free_nodes_count_before;
            for(size_type j = 0; j != num_elems; ++j){
               void *new_node = &free_nodes.front();
               free_nodes.pop_front();
               chain.push_back(new_node);
            }

            if(free_nodes.empty()){
               m_block_multiset.erase(m_block_multiset.begin());
            }
            i += num_elems;
         }
      }
      catch(...){
         this->deallocate_nodes(boost::move(chain));
         throw;
      }
      priv_invariants();
      return boost::move(chain);
   }

   //!Deallocates a linked list of nodes. Never throws
   void deallocate_nodes(multiallocation_chain nodes)
   {
      this->priv_reinsert_nodes_in_block(nodes, nodes.size());
      if(m_totally_free_blocks > m_max_free_blocks){
         this->priv_deallocate_free_blocks(m_max_free_blocks);
      }
   }

   void deallocate_free_blocks()
   {  this->priv_deallocate_free_blocks(0);   }

   size_type num_free_nodes()
   {
      typedef typename block_multiset_t::const_iterator citerator;
      size_type count = 0;
      citerator it (m_block_multiset.begin()), itend(m_block_multiset.end());
      for(; it != itend; ++it){
         count += it->free_nodes.size();
      }
      return count;
   }

   void swap(private_adaptive_node_pool_impl &other)
   {
      BOOST_ASSERT(m_max_free_blocks == other.m_max_free_blocks);
      BOOST_ASSERT(m_real_node_size == other.m_real_node_size);
      BOOST_ASSERT(m_real_block_alignment == other.m_real_block_alignment);
      BOOST_ASSERT(m_real_num_node == other.m_real_num_node);
      std::swap(mp_segment_mngr_base, other.mp_segment_mngr_base);
      std::swap(m_totally_free_blocks, other.m_totally_free_blocks);
      m_block_multiset.swap(other.m_block_multiset);
   }

   //Deprecated, use deallocate_free_blocks
   void deallocate_free_chunks()
   {  this->priv_deallocate_free_blocks(0);   }

   private:
   void priv_deallocate_free_blocks(size_type max_free_blocks)
   {
      priv_invariants();
      //Now check if we've reached the free nodes limit
      //and check if we have free blocks. If so, deallocate as much
      //as we can to stay below the limit
      for( block_iterator itend = m_block_multiset.end()
         ; m_totally_free_blocks > max_free_blocks
         ; --m_totally_free_blocks
         ){
         BOOST_ASSERT(!m_block_multiset.empty());
         block_iterator it = itend;
         --it;
         BOOST_ASSERT(it->free_nodes.size() == m_real_num_node);
         m_block_multiset.erase_and_dispose(it, block_destroyer(this));
      }
   }

   void priv_reinsert_nodes_in_block(multiallocation_chain &chain, size_type n)
   {
      block_iterator block_it(m_block_multiset.end());
      while(n--){
         void *pElem = container_detail::to_raw_pointer(chain.front());
         chain.pop_front();
         priv_invariants();
         block_info_t *block_info = this->priv_block_from_node(pElem);
         BOOST_ASSERT(block_info->free_nodes.size() < m_real_num_node);
         //We put the node at the beginning of the free node list
         node_t * to_deallocate = static_cast<node_t*>(pElem);
         block_info->free_nodes.push_front(*to_deallocate);

         block_iterator this_block(block_multiset_t::s_iterator_to(*block_info));
         block_iterator next_block(this_block);
         ++next_block;

         //Cache the free nodes from the block
         size_type this_block_free_nodes = this_block->free_nodes.size();

         if(this_block_free_nodes == 1){
            m_block_multiset.insert(m_block_multiset.begin(), *block_info);
         }
         else{
            block_iterator next_block(this_block);
            ++next_block;
            if(next_block != block_it){
               size_type next_free_nodes = next_block->free_nodes.size();
               if(this_block_free_nodes > next_free_nodes){
                  //Now move the block to the new position
                  m_block_multiset.erase(this_block);
                  m_block_multiset.insert(*block_info);
               }
            }
         }
         //Update free block count
         if(this_block_free_nodes == m_real_num_node){
            ++m_totally_free_blocks;
         }
         priv_invariants();
      }
   }

   node_t *priv_take_first_node()
   {
      BOOST_ASSERT(m_block_multiset.begin() != m_block_multiset.end());
      //We take the first free node the multiset can't be empty
      free_nodes_t &free_nodes = m_block_multiset.begin()->free_nodes;
      node_t *first_node = &free_nodes.front();
      const size_type free_nodes_count = free_nodes.size();
      BOOST_ASSERT(0 != free_nodes_count);
      free_nodes.pop_front();
      if(free_nodes_count == 1){
         m_block_multiset.erase(m_block_multiset.begin());
      }
      else if(free_nodes_count == m_real_num_node){
         --m_totally_free_blocks;
      }
      priv_invariants();
      return first_node;
   }

   class block_destroyer;
   friend class block_destroyer;

   class block_destroyer
   {
      public:
      block_destroyer(const this_type *impl)
         :  mp_impl(impl)
      {}

      void operator()(typename block_multiset_t::pointer to_deallocate)
      {  return this->do_destroy(to_deallocate, IsAlignOnly()); }

      private:
      void do_destroy(typename block_multiset_t::pointer to_deallocate, AlignOnlyTrue)
      {
         size_type free_nodes = to_deallocate->free_nodes.size();
         (void)free_nodes;
         BOOST_ASSERT(free_nodes == mp_impl->m_real_num_node);
         mp_impl->mp_segment_mngr_base->deallocate(to_deallocate);
      }

      void do_destroy(typename block_multiset_t::pointer to_deallocate, AlignOnlyFalse)
      {
         size_type free_nodes = to_deallocate->free_nodes.size();
         (void)free_nodes;
         BOOST_ASSERT(free_nodes == mp_impl->m_real_num_node);
         BOOST_ASSERT(0 == to_deallocate->hdr_offset);
         hdr_offset_holder *hdr_off_holder = mp_impl->priv_first_subblock_from_block(container_detail::to_raw_pointer(to_deallocate));
         mp_impl->mp_segment_mngr_base->deallocate(hdr_off_holder);
      }

      const this_type *mp_impl;
   };

   //This macro will activate invariant checking. Slow, but helpful for debugging the code.
   //#define BOOST_CONTAINER_ADAPTIVE_NODE_POOL_CHECK_INVARIANTS
   void priv_invariants()
   #ifdef BOOST_CONTAINER_ADAPTIVE_NODE_POOL_CHECK_INVARIANTS
   #undef BOOST_CONTAINER_ADAPTIVE_NODE_POOL_CHECK_INVARIANTS
   {
      //We iterate through the block tree to free the memory
      block_iterator it(m_block_multiset.begin()), 
                     itend(m_block_multiset.end()), to_deallocate;
      if(it != itend){
         for(++it; it != itend; ++it){
            block_iterator prev(it);
            --prev;
            size_type sp = prev->free_nodes.size(),
                        si = it->free_nodes.size();
            BOOST_ASSERT(sp <= si);
            (void)sp;   (void)si;
         }
      }
      //Check that the total free nodes are correct
      it    = m_block_multiset.begin();
      itend = m_block_multiset.end();
      size_type total_free_nodes = 0;
      for(; it != itend; ++it){
         total_free_nodes += it->free_nodes.size();
      }
      BOOST_ASSERT(total_free_nodes >= m_totally_free_blocks*m_real_num_node);

      //Check that the total totally free blocks are correct
      it    = m_block_multiset.begin();
      itend = m_block_multiset.end();
      total_free = 0;
      for(; it != itend; ++it){
         total_free += it->free_nodes.size() == m_real_num_node;
      }
      BOOST_ASSERT(total_free >= m_totally_free_blocks);

      if(!AlignOnly){
         //Check that header offsets are correct
         it = m_block_multiset.begin();
         for(; it != itend; ++it){
            hdr_offset_holder *hdr_off_holder = priv_first_subblock_from_block(&*it);
            for(size_type i = 0, max = m_num_subblocks; i < max; ++i){
               BOOST_ASSERT(hdr_off_holder->hdr_offset == size_type(reinterpret_cast<char*>(&*it)- reinterpret_cast<char*>(hdr_off_holder)));
               BOOST_ASSERT(0 == ((size_type)hdr_off_holder & (m_real_block_alignment - 1)));
               BOOST_ASSERT(0 == (hdr_off_holder->hdr_offset & (m_real_block_alignment - 1)));
               hdr_off_holder = reinterpret_cast<hdr_offset_holder *>(reinterpret_cast<char*>(hdr_off_holder) + m_real_block_alignment);
            }
         }
      }
   }
   #else
   {} //empty
   #endif

   //!Deallocates all used memory. Never throws
   void priv_clear()
   {
      #ifndef NDEBUG
      block_iterator it    = m_block_multiset.begin();
      block_iterator itend = m_block_multiset.end();
      size_type num_free_nodes = 0;
      for(; it != itend; ++it){
         //Check for memory leak
         BOOST_ASSERT(it->free_nodes.size() == m_real_num_node);
         ++num_free_nodes;
      }
      BOOST_ASSERT(num_free_nodes == m_totally_free_blocks);
      #endif
      //Check for memory leaks
      priv_invariants();
      m_block_multiset.clear_and_dispose(block_destroyer(this));
      m_totally_free_blocks = 0;
   }

   block_info_t *priv_block_from_node(void *node, AlignOnlyFalse) const
   {
      hdr_offset_holder *hdr_off_holder =
         reinterpret_cast<hdr_offset_holder*>((std::size_t)node & size_type(~(m_real_block_alignment - 1)));
      BOOST_ASSERT(0 == ((std::size_t)hdr_off_holder & (m_real_block_alignment - 1)));
      BOOST_ASSERT(0 == (hdr_off_holder->hdr_offset & (m_real_block_alignment - 1)));
      block_info_t *block = reinterpret_cast<block_info_t *>
         (reinterpret_cast<char*>(hdr_off_holder) + hdr_off_holder->hdr_offset);
      BOOST_ASSERT(block->hdr_offset == 0);
      return block;
   }

   block_info_t *priv_block_from_node(void *node, AlignOnlyTrue) const
   {
      return (block_info_t *)((std::size_t)node & std::size_t(~(m_real_block_alignment - 1)));
   }

   block_info_t *priv_block_from_node(void *node) const
   {  return priv_block_from_node(node, IsAlignOnly());   }

   hdr_offset_holder *priv_first_subblock_from_block(block_info_t *block) const
   {
      hdr_offset_holder *hdr_off_holder = reinterpret_cast<hdr_offset_holder*>
            (reinterpret_cast<char*>(block) - (m_num_subblocks-1)*m_real_block_alignment);
      BOOST_ASSERT(hdr_off_holder->hdr_offset == size_type(reinterpret_cast<char*>(block) - reinterpret_cast<char*>(hdr_off_holder)));
     BOOST_ASSERT(0 == ((std::size_t)hdr_off_holder & (m_real_block_alignment - 1)));
      BOOST_ASSERT(0 == (hdr_off_holder->hdr_offset & (m_real_block_alignment - 1)));
      return hdr_off_holder;
   }

   //!Allocates a several blocks of nodes. Can throw
   void priv_alloc_block(size_type n, AlignOnlyTrue)
   {
      size_type real_block_size = m_real_block_alignment - PayloadPerAllocation;
      for(size_type i = 0; i != n; ++i){
         //We allocate a new NodeBlock and put it the last
         //element of the tree
         char *mem_address = static_cast<char*>
            (mp_segment_mngr_base->allocate_aligned(real_block_size, m_real_block_alignment));
         if(!mem_address)   throw std::bad_alloc();
         ++m_totally_free_blocks;
         block_info_t *c_info = new(mem_address)block_info_t();
         m_block_multiset.insert(m_block_multiset.end(), *c_info);
         
         mem_address += HdrSize;
         //We initialize all Nodes in Node Block to insert 
         //them in the free Node list
         typename free_nodes_t::iterator prev_insert_pos = c_info->free_nodes.before_begin();
         for(size_type i = 0; i < m_real_num_node; ++i){
            prev_insert_pos = c_info->free_nodes.insert_after(prev_insert_pos, *(node_t*)mem_address);
            mem_address   += m_real_node_size;
         }
      }
   }

   void priv_alloc_block(size_type n, AlignOnlyFalse)
   {
      size_type real_block_size = m_real_block_alignment*m_num_subblocks - PayloadPerAllocation;
      size_type elements_per_subblock = (m_real_block_alignment - HdrOffsetSize)/m_real_node_size;
      size_type hdr_subblock_elements = (m_real_block_alignment - HdrSize - PayloadPerAllocation)/m_real_node_size;

      for(size_type i = 0; i != n; ++i){
         //We allocate a new NodeBlock and put it the last
         //element of the tree
         char *mem_address = static_cast<char*>
            (mp_segment_mngr_base->allocate_aligned(real_block_size, m_real_block_alignment));
         if(!mem_address)   throw std::bad_alloc();
         ++m_totally_free_blocks;

         //First initialize header information on the last subblock
         char *hdr_addr = mem_address + m_real_block_alignment*(m_num_subblocks-1);
         block_info_t *c_info = new(hdr_addr)block_info_t();
         //Some structural checks
         BOOST_ASSERT(static_cast<void*>(&static_cast<hdr_offset_holder*>(c_info)->hdr_offset) ==
                static_cast<void*>(c_info));
         typename free_nodes_t::iterator prev_insert_pos = c_info->free_nodes.before_begin();
         for( size_type subblock = 0, maxsubblock = m_num_subblocks - 1
            ; subblock < maxsubblock
            ; ++subblock, mem_address += m_real_block_alignment){
            //Initialize header offset mark
            new(mem_address) hdr_offset_holder(size_type(hdr_addr - mem_address));
            char *pNode = mem_address + HdrOffsetSize;
            for(size_type i = 0; i < elements_per_subblock; ++i){
               prev_insert_pos = c_info->free_nodes.insert_after(prev_insert_pos, *new (pNode) node_t);
               pNode   += m_real_node_size;
            }
         }
         {
            char *pNode = hdr_addr + HdrSize;
            //We initialize all Nodes in Node Block to insert 
            //them in the free Node list
            for(size_type i = 0; i < hdr_subblock_elements; ++i){
               prev_insert_pos = c_info->free_nodes.insert_after(prev_insert_pos, *new (pNode) node_t);
               pNode   += m_real_node_size;
            }
         }
         //Insert the block after the free node list is full
         m_block_multiset.insert(m_block_multiset.end(), *c_info);
      }
   }

   //!Allocates a block of nodes. Can throw std::bad_alloc
   void priv_alloc_block(size_type n)
   {  return priv_alloc_block(n, IsAlignOnly());   }

   private:
   typedef typename boost::intrusive::pointer_traits
      <void_pointer>::template rebind_pointer<segment_manager_base_type>::type   segment_mngr_base_ptr_t;
   const size_type m_max_free_blocks;
   const size_type m_real_node_size;
   //Round the size to a power of two value.
   //This is the total memory size (including payload) that we want to
   //allocate from the general-purpose allocator
   const size_type m_real_block_alignment;
   size_type m_num_subblocks;
   //This is the real number of nodes per block
   //const
   size_type m_real_num_node;
   segment_mngr_base_ptr_t                mp_segment_mngr_base;   //Segment manager
   block_multiset_t                       m_block_multiset;       //Intrusive block list
   size_type                            m_totally_free_blocks;  //Free blocks
};

}  //namespace container_detail {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_ADAPTIVE_NODE_POOL_IMPL_HPP
