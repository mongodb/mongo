//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_DETAIL_MEM_ALGO_COMMON_HPP
#define BOOST_INTERPROCESS_DETAIL_MEM_ALGO_COMMON_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>

// interprocess
#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/containers/allocation_type.hpp>
// interprocess/detail
#include <boost/interprocess/detail/math_functions.hpp>
#include <boost/interprocess/detail/min_max.hpp>
#include <boost/interprocess/detail/type_traits.hpp>
#include <boost/interprocess/detail/utilities.hpp>
// container/detail
#include <boost/container/detail/multiallocation_chain.hpp>
#include <boost/container/detail/placement_new.hpp>
// move
#include <boost/move/utility_core.hpp>
// move/detail
#include <boost/move/detail/force_ptr.hpp>
// other boost
#include <boost/assert.hpp>

//!\file
//!Implements common operations for memory algorithms.

namespace boost {
namespace interprocess {
namespace ipcdetail {

template<class VoidPointer>
class basic_multiallocation_chain
   : public boost::container::dtl::
      basic_multiallocation_chain<VoidPointer>
{
   BOOST_MOVABLE_BUT_NOT_COPYABLE(basic_multiallocation_chain)
   typedef boost::container::dtl::
      basic_multiallocation_chain<VoidPointer> base_t;
   public:

   basic_multiallocation_chain()
      :  base_t()
   {}

   basic_multiallocation_chain(BOOST_RV_REF(basic_multiallocation_chain) other)
      :  base_t(::boost::move(static_cast<base_t&>(other)))
   {}

   basic_multiallocation_chain& operator=(BOOST_RV_REF(basic_multiallocation_chain) other)
   {
      this->base_t::operator=(::boost::move(static_cast<base_t&>(other)));
      return *this;
   }

   void *pop_front()
   {
      return boost::interprocess::ipcdetail::to_raw_pointer(this->base_t::pop_front());
   }
};

//!This class implements several allocation functions shared by different algorithms
//!(aligned allocation, multiple allocation...).
template<class MemoryAlgorithm>
class memory_algorithm_common
{
   public:
   typedef typename MemoryAlgorithm::void_pointer              void_pointer;
   typedef typename MemoryAlgorithm::block_ctrl                block_ctrl;
   typedef typename MemoryAlgorithm::multiallocation_chain     multiallocation_chain;
   typedef memory_algorithm_common<MemoryAlgorithm>            this_type;
   typedef typename MemoryAlgorithm::size_type                 size_type;

   static const size_type Alignment              = MemoryAlgorithm::Alignment;
   static const size_type AllocatedCtrlBytes     = MemoryAlgorithm::AllocatedCtrlBytes;
   static const size_type AllocatedCtrlUnits     = MemoryAlgorithm::AllocatedCtrlUnits;
   static const size_type BlockCtrlBytes         = MemoryAlgorithm::BlockCtrlBytes;
   static const size_type BlockCtrlUnits         = MemoryAlgorithm::BlockCtrlUnits;
   static const size_type UsableByPreviousChunk  = MemoryAlgorithm::UsableByPreviousChunk;

   static void assert_alignment(const void *ptr)
   {  assert_alignment((std::size_t)ptr); }

   static void assert_alignment(size_type uint_ptr)
   {
      (void)uint_ptr;
      BOOST_ASSERT(uint_ptr % Alignment == 0);
   }

   static bool check_alignment(const void *ptr)
   {  return (((std::size_t)ptr) % Alignment == 0);   }

   static size_type ceil_units(size_type size)
   {  return get_rounded_size(size, Alignment)/Alignment; }

   static size_type floor_units(size_type size)
   {  return size/Alignment;  }

   static size_type user_buffer_ceil_units(size_type size)
   {
      if(size <= UsableByPreviousChunk)
         return 0;
      return ceil_units(size - UsableByPreviousChunk);
   }

   static size_type multiple_of_units(size_type size)
   {  return get_rounded_size(size, Alignment);  }

   static void allocate_many
      (MemoryAlgorithm *memory_algo, size_type elem_bytes, size_type n_elements, multiallocation_chain &chain)
   {
      return this_type::priv_allocate_many(memory_algo, &elem_bytes, n_elements, 0, chain);
   }

   static void deallocate_many(MemoryAlgorithm *memory_algo, multiallocation_chain &chain)
   {
      return this_type::priv_deallocate_many(memory_algo, chain);
   }

   static bool calculate_lcm_and_needs_backwards_lcmed
      (size_type backwards_multiple, size_type received_size, size_type size_to_achieve,
      size_type &lcm_out, size_type &needs_backwards_lcmed_out)
   {
      // Now calculate lcm_val
      size_type max = backwards_multiple;
      size_type min = Alignment;
      size_type needs_backwards;
      size_type needs_backwards_lcmed;
      size_type lcm_val;
      size_type current_forward;
      //Swap if necessary
      if(max < min){
         size_type tmp = min;
         min = max;
         max = tmp;
      }
      //Check if it's power of two
      if((backwards_multiple & (backwards_multiple-1)) == 0){
         if(0 != (size_to_achieve & ((backwards_multiple-1)))){
            return false;
         }

         lcm_val = max;
         //If we want to use minbytes data to get a buffer between maxbytes
         //and minbytes if maxbytes can't be achieved, calculate the
         //biggest of all possibilities
         current_forward = get_truncated_size_po2(received_size, backwards_multiple);
         needs_backwards = size_to_achieve - current_forward;
         BOOST_ASSERT((needs_backwards % backwards_multiple) == 0);
         needs_backwards_lcmed = get_rounded_size_po2(needs_backwards, lcm_val);
         lcm_out = lcm_val;
         needs_backwards_lcmed_out = needs_backwards_lcmed;
         return true;
      }
      //Check if it's multiple of alignment
      else if((backwards_multiple & (Alignment - 1u)) == 0){
         lcm_val = backwards_multiple;
         current_forward = get_truncated_size(received_size, backwards_multiple);
         //No need to round needs_backwards because backwards_multiple == lcm_val
         needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
         BOOST_ASSERT((needs_backwards_lcmed & (Alignment - 1u)) == 0);
         lcm_out = lcm_val;
         needs_backwards_lcmed_out = needs_backwards_lcmed;
         return true;
      }
      //Check if it's multiple of the half of the alignmment
      else if((backwards_multiple & ((Alignment/2u) - 1u)) == 0){
         lcm_val = backwards_multiple*2u;
         current_forward = get_truncated_size(received_size, backwards_multiple);
         needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
         if(0 != (needs_backwards_lcmed & (Alignment-1)))
         //while(0 != (needs_backwards_lcmed & (Alignment-1)))
            needs_backwards_lcmed += backwards_multiple;
         BOOST_ASSERT((needs_backwards_lcmed % lcm_val) == 0);
         lcm_out = lcm_val;
         needs_backwards_lcmed_out = needs_backwards_lcmed;
         return true;
      }
      //Check if it's multiple of the quarter of the alignmment
      else if((backwards_multiple & ((Alignment/4u) - 1u)) == 0){
         size_type remainder;
         lcm_val = backwards_multiple*4u;
         current_forward = get_truncated_size(received_size, backwards_multiple);
         needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
         //while(0 != (needs_backwards_lcmed & (Alignment-1)))
            //needs_backwards_lcmed += backwards_multiple;
         if(0 != (remainder = ((needs_backwards_lcmed & (Alignment-1))>>(Alignment/8u)))){
            if(backwards_multiple & Alignment/2u){
               needs_backwards_lcmed += (remainder)*backwards_multiple;
            }
            else{
               needs_backwards_lcmed += (4-remainder)*backwards_multiple;
            }
         }
         BOOST_ASSERT((needs_backwards_lcmed % lcm_val) == 0);
         lcm_out = lcm_val;
         needs_backwards_lcmed_out = needs_backwards_lcmed;
         return true;
      }
      else{
         lcm_val = lcm(max, min);
      }
      //If we want to use minbytes data to get a buffer between maxbytes
      //and minbytes if maxbytes can't be achieved, calculate the
      //biggest of all possibilities
      current_forward = get_truncated_size(received_size, backwards_multiple);
      needs_backwards = size_to_achieve - current_forward;
      BOOST_ASSERT((needs_backwards % backwards_multiple) == 0);
      needs_backwards_lcmed = get_rounded_size(needs_backwards, lcm_val);
      lcm_out = lcm_val;
      needs_backwards_lcmed_out = needs_backwards_lcmed;
      return true;
   }

   static void allocate_many
      ( MemoryAlgorithm *memory_algo
      , const size_type *elem_sizes
      , size_type n_elements
      , size_type sizeof_element
      , multiallocation_chain &chain)
   {
      this_type::priv_allocate_many(memory_algo, elem_sizes, n_elements, sizeof_element, chain);
   }

   static void* allocate_aligned
      (MemoryAlgorithm * const memory_algo, const size_type nbytes, const size_type alignment)
   {

      //Ensure power of 2
      const bool alignment_ok = (alignment & (alignment - 1u)) == 0;
      if (!alignment_ok){
         //Alignment is not power of two
         BOOST_ASSERT(alignment_ok);
         return 0;
      }

      if(alignment <= Alignment){
         size_type real_size = nbytes;
         void *ignore_reuse = 0;
         return memory_algo->priv_allocate
            (boost::interprocess::allocate_new, nbytes, real_size, ignore_reuse);
      }

      //To fulfill user's request we need at least min_user_units
      size_type needed_units = user_buffer_ceil_units(nbytes);
      //However, there is a minimum allocation unit count (BlockCtrlUnits) to be able to deallocate the buffer,
      //The allocation will give us a part of it (AllocatedCtrlUnits) so (BlockCtrlUnits - AllocatedCtrlUnits)
      //is the minimum ammount of blocks we need to allocate.
      needed_units += max_value(needed_units, BlockCtrlUnits - AllocatedCtrlUnits);
      //If we need to align, we need to at least move enough to create a new block at the beginning
      //that can be marked as free, so we need BlockCtrlUnits units for that
      needed_units += BlockCtrlUnits;
      //Finally, we need to add extra space to be sure we will find an aligned address
      needed_units += (alignment - Alignment)/Alignment;

      //Transform units to bytes
      const size_type request = needed_units*Alignment + UsableByPreviousChunk;

      //Now allocate the buffer
      size_type real_size = request;
      void *ignore_reuse = 0;
      void *const buffer = memory_algo->priv_allocate(boost::interprocess::allocate_new, request, real_size, ignore_reuse);
      if(!buffer){
         return 0;
      }
      else if ((((std::size_t)(buffer)) & (alignment-1)) == 0){
         //If we are lucky and the buffer is aligned, just split it and
         //return the high part
         block_ctrl *const first  = memory_algo->priv_get_block(buffer);
         const size_type orig_first_units = first->m_size;
         const size_type first_min_units =
            max_value(user_buffer_ceil_units(nbytes) + AllocatedCtrlUnits, size_type(BlockCtrlUnits));
         //We can create a new block in the end of the segment
         if(orig_first_units >= (first_min_units + BlockCtrlUnits)){
            block_ctrl *second =  move_detail::force_ptr<block_ctrl*>
               (reinterpret_cast<char*>(first) + Alignment*first_min_units);
            //Update first size
            first->m_size  = first_min_units & block_ctrl::size_mask;
            memory_algo->priv_mark_new_allocated_block(first);

            //Deallocate the remaining memory
            second->m_size = (orig_first_units - first_min_units) & block_ctrl::size_mask;
            memory_algo->priv_mark_new_allocated_block(second);
            memory_algo->priv_deallocate(memory_algo->priv_get_user_buffer(second));
         }
         return buffer;
      }

      //Now obtain the address of the allocated block
      block_ctrl* const first = memory_algo->priv_get_block(buffer);
      //The block must be marked as allocated
      BOOST_ASSERT(memory_algo->priv_is_allocated_block(first));
      //Assert allocated block has at least the desired size
      BOOST_ASSERT(first->m_size >= (needed_units + AllocatedCtrlUnits));
      //Assert allocated block can be splitted in the two blocks
      BOOST_ASSERT(first->m_size >= 2 * BlockCtrlUnits);

      //Buffer is not overaligned, so find the aligned part

      // BCB: BlockControlBytes
      // ACB: AllocatedControlBytes (<= BlockControlBytes)
      //
      //  __________> Block control ("first")
      // |           _________> Block control ("second")
      // |          |      ___> usr_buf, overaligned
      // |          |     |
      //  -----------------------------------------------------
      // | BCB+more | ACB |
      //  -----------------------------------------------------
      char *const usr_buf = reinterpret_cast<char*>
         (reinterpret_cast<std::size_t>(static_cast<char*>(buffer)
            + BlockCtrlBytes                 //Minimum to create a free block at the beginning
            + alignment - 1) & -alignment);  //This is the alignment trick

      //Assert the user buffer is inside the allocated range
      BOOST_ASSERT(usr_buf <= (reinterpret_cast<char*>(first) + first->m_size*Alignment));
      //Assert all user data is inside the allocated range
      BOOST_ASSERT((usr_buf + nbytes) <= (reinterpret_cast<char*>(first) + first->m_size*Alignment + UsableByPreviousChunk));

      //Set the new size of the secone block
      const size_type orig_first_units = first->m_size;

      block_ctrl* const second = memory_algo->priv_get_block(usr_buf);

      //Update first block size until second block starts and deallocate it
      const size_type final_first_units =
         size_type(reinterpret_cast<char*>(second) - reinterpret_cast<char*>(first))/Alignment & block_ctrl::size_mask;

      //Now check if we can create a new buffer in the end
      //
      //  _______________________> "first" (free block)
      // |           ____________> "second" block
      // |          |      ______> user data aligned here (usr_buf)
      // |          |     |            ____> optional "third" (free block)
      //  ----------|-----|-----------|------------------------------
      // | BCB+more | ACB | user_data | BCB |
      //  -----------------------------------------------------
      //This size will be the minimum size to be able to create a
      //new block in the end.
      const size_type orig_second_units = orig_first_units - final_first_units;
      const size_type second_min_units = max_value( size_type(BlockCtrlUnits)
                                                  , user_buffer_ceil_units(nbytes) + AllocatedCtrlUnits );

      //Check if we can create a new free block (of size BlockCtrlUnits) at the end of the segment
      if(orig_second_units >= (second_min_units + BlockCtrlUnits)){
         //Now obtain the address of the end block
         block_ctrl *const third = ::new (reinterpret_cast<char*>(second) + Alignment*second_min_units, boost_container_new_t()) block_ctrl;
         second->m_size = second_min_units & block_ctrl::size_mask;
         third->m_size  = (orig_second_units - second->m_size) & block_ctrl::size_mask;
         BOOST_ASSERT(third->m_size >= BlockCtrlUnits);
         memory_algo->priv_mark_new_allocated_block(second);
         memory_algo->priv_mark_new_allocated_block(third);
         //We can deallocate third block because the previous "second" is properly set
         memory_algo->priv_deallocate(memory_algo->priv_get_user_buffer(third));
      }
      else{
         second->m_size = orig_second_units & block_ctrl::size_mask;
         BOOST_ASSERT(second->m_size >= BlockCtrlUnits);
         memory_algo->priv_mark_new_allocated_block(second);
      }

      //We can deallocate first block because the next "second" is properly set
      first->m_size = final_first_units & block_ctrl::size_mask;
      //Now mark second's previous allocated flag as allocated
      memory_algo->priv_mark_new_allocated_block(first);
      memory_algo->priv_deallocate(memory_algo->priv_get_user_buffer(first));

      //Make sure all user data fits
      BOOST_ASSERT((reinterpret_cast<char*>(usr_buf) + nbytes) <= (reinterpret_cast<char*>(second) + second->m_size*Alignment + UsableByPreviousChunk));
      //Make sure user data is properly aligned
      BOOST_ASSERT(0 == ((std::size_t)usr_buf & (alignment-1u)));
      return usr_buf;
   }

   static bool try_shrink
      (MemoryAlgorithm *memory_algo, void *ptr
      ,const size_type max_size, size_type &received_size)
   {
      size_type const preferred_size = received_size;
      (void)memory_algo;
      //Obtain the real block
      block_ctrl *block = memory_algo->priv_get_block(ptr);
      size_type old_block_units = (size_type)block->m_size;

      //The block must be marked as allocated
      BOOST_ASSERT(memory_algo->priv_is_allocated_block(block));

      //Check if alignment and block size are right
      assert_alignment(ptr);

      //Put this to a safe value
      received_size = (old_block_units - AllocatedCtrlUnits)*Alignment + UsableByPreviousChunk;

      //Now translate it to Alignment units
      const size_type max_user_units       = floor_units(max_size - UsableByPreviousChunk);
      const size_type preferred_user_units = ceil_units(preferred_size - UsableByPreviousChunk);

      //Check if rounded max and preferred are possible correct
      if(max_user_units < preferred_user_units)
         return false;

      //Check if the block is smaller than the requested minimum
      size_type old_user_units = old_block_units - AllocatedCtrlUnits;

      if(old_user_units < preferred_user_units)
         return false;

      //If the block is smaller than the requested minimum
      if(old_user_units == preferred_user_units)
         return true;

      size_type shrunk_user_units =
         ((BlockCtrlUnits - AllocatedCtrlUnits) >= preferred_user_units)
         ? (BlockCtrlUnits - AllocatedCtrlUnits)
         : preferred_user_units;

      //Some parameter checks
      if(max_user_units < shrunk_user_units)
         return false;

      //We must be able to create at least a new empty block
      if((old_user_units - shrunk_user_units) < BlockCtrlUnits ){
         return false;
      }

      //Update new size
      received_size = shrunk_user_units*Alignment + UsableByPreviousChunk;
      return true;
   }

   static bool shrink
      (MemoryAlgorithm *memory_algo, void *ptr
      ,const size_type max_size, size_type &received_size)
   {
      size_type const preferred_size = received_size;
      //Obtain the real block
      block_ctrl *block = memory_algo->priv_get_block(ptr);
      size_type old_block_units = (size_type)block->m_size;

      if(!try_shrink(memory_algo, ptr, max_size, received_size)){
         return false;
      }

      //Check if the old size was just the shrunk size (no splitting)
      if((old_block_units - AllocatedCtrlUnits) == ceil_units(preferred_size - UsableByPreviousChunk))
         return true;

      //Now we can just rewrite the size of the old buffer
      block->m_size = ((received_size-UsableByPreviousChunk)/Alignment + AllocatedCtrlUnits) & block_ctrl::size_mask;
      BOOST_ASSERT(block->m_size >= BlockCtrlUnits);

      //We create the new block
      block_ctrl *new_block = move_detail::force_ptr<block_ctrl*>
                  (reinterpret_cast<char*>(block) + block->m_size*Alignment);
      //Write control data to simulate this new block was previously allocated
      //and deallocate it
      new_block->m_size = (old_block_units - block->m_size) & block_ctrl::size_mask;
      BOOST_ASSERT(new_block->m_size >= BlockCtrlUnits);
      memory_algo->priv_mark_new_allocated_block(block);
      memory_algo->priv_mark_new_allocated_block(new_block);
      memory_algo->priv_deallocate(memory_algo->priv_get_user_buffer(new_block));
      return true;
   }

   private:
   static void priv_allocate_many
      ( MemoryAlgorithm *memory_algo
      , const size_type *elem_sizes
      , size_type n_elements
      , size_type sizeof_element
      , multiallocation_chain &chain)
   {
      //Note: sizeof_element == 0 indicates that we want to
      //allocate n_elements of the same size "*elem_sizes"

      //Calculate the total size of all requests
      size_type total_request_units = 0;
      size_type elem_units = 0;
      const size_type ptr_size_units = memory_algo->priv_get_total_units(sizeof(void_pointer));
      if(!sizeof_element){
         elem_units = memory_algo->priv_get_total_units(*elem_sizes);
         elem_units = ptr_size_units > elem_units ? ptr_size_units : elem_units;
         total_request_units = n_elements*elem_units;
      }
      else{
         for(size_type i = 0; i < n_elements; ++i){
            if(multiplication_overflows(elem_sizes[i], sizeof_element)){
               total_request_units = 0;
               break;
            }
            elem_units = memory_algo->priv_get_total_units(elem_sizes[i]*sizeof_element);
            elem_units = ptr_size_units > elem_units ? ptr_size_units : elem_units;
            if(sum_overflows(total_request_units, elem_units)){
               total_request_units = 0;
               break;
            }
            total_request_units += elem_units;
         }
      }

      if(total_request_units && !multiplication_overflows(total_request_units, Alignment)){
         size_type low_idx = 0;
         while(low_idx < n_elements){
            size_type total_bytes = total_request_units*Alignment - AllocatedCtrlBytes + UsableByPreviousChunk;
            size_type min_allocation = (!sizeof_element)
               ?  elem_units
               :  memory_algo->priv_get_total_units(elem_sizes[low_idx]*sizeof_element);
            min_allocation = min_allocation*Alignment - AllocatedCtrlBytes + UsableByPreviousChunk;

            size_type received_size = total_bytes;
            void *ignore_reuse = 0;
            void *ret = memory_algo->priv_allocate
               (boost::interprocess::allocate_new, min_allocation, received_size, ignore_reuse);
            if(!ret){
               break;
            }

            block_ctrl *block = memory_algo->priv_get_block(ret);
            size_type received_units = (size_type)block->m_size;
            char *block_address = reinterpret_cast<char*>(block);

            size_type total_used_units = 0;
            while(total_used_units < received_units){
               if(sizeof_element){
                  elem_units = memory_algo->priv_get_total_units(elem_sizes[low_idx]*sizeof_element);
                  elem_units = ptr_size_units > elem_units ? ptr_size_units : elem_units;
               }
               if(total_used_units + elem_units > received_units)
                  break;
               total_request_units -= elem_units;
               //This is the position where the new block must be created
               block_ctrl *new_block = move_detail::force_ptr<block_ctrl*>(block_address);
               assert_alignment(new_block);

               //The last block should take all the remaining space
               if((low_idx + 1) == n_elements ||
                  (total_used_units + elem_units +
                  ((!sizeof_element)
                     ? elem_units
               : max_value(memory_algo->priv_get_total_units(elem_sizes[low_idx+1]*sizeof_element), ptr_size_units))
                   > received_units)){
                  //By default, the new block will use the rest of the buffer
                  new_block->m_size = (received_units - total_used_units) & block_ctrl::size_mask;
                  memory_algo->priv_mark_new_allocated_block(new_block);

                  //If the remaining units are bigger than needed and we can
                  //split it obtaining a new free memory block do it.
                  if((received_units - total_used_units) >= (elem_units + MemoryAlgorithm::BlockCtrlUnits)){
                     size_type shrunk_request = elem_units*Alignment - AllocatedCtrlBytes + UsableByPreviousChunk;
                     size_type shrunk_received = shrunk_request;
                     bool shrink_ok = shrink
                           (memory_algo
                           ,memory_algo->priv_get_user_buffer(new_block)
                           ,shrunk_request
                           ,shrunk_received);
                     (void)shrink_ok;
                     //Shrink must always succeed with passed parameters
                     BOOST_ASSERT(shrink_ok);
                     //Some sanity checks
                     BOOST_ASSERT(shrunk_request == shrunk_received);
                     BOOST_ASSERT(elem_units == ((shrunk_request-UsableByPreviousChunk)/Alignment + AllocatedCtrlUnits));
                     //"new_block->m_size" must have been reduced to elem_units by "shrink"
                     BOOST_ASSERT(new_block->m_size == elem_units);
                     //Now update the total received units with the reduction
                     received_units = elem_units + total_used_units;
                  }
               }
               else{
                  new_block->m_size = elem_units & block_ctrl::size_mask;
                  memory_algo->priv_mark_new_allocated_block(new_block);
               }

               block_address += new_block->m_size*Alignment;
               total_used_units += (size_type)new_block->m_size;
               //Check we have enough room to overwrite the intrusive pointer
               BOOST_ASSERT((new_block->m_size*Alignment - AllocatedCtrlUnits) >= sizeof(void_pointer));
               void_pointer p = ::new(memory_algo->priv_get_user_buffer(new_block), boost_container_new_t())void_pointer(0);
               chain.push_back(p);
               ++low_idx;
            }
            //Sanity check
            BOOST_ASSERT(total_used_units == received_units);
         }

         if(low_idx != n_elements){
            priv_deallocate_many(memory_algo, chain);
         }
      }
   }

   static void priv_deallocate_many(MemoryAlgorithm *memory_algo, multiallocation_chain &chain)
   {
      while(!chain.empty()){
         memory_algo->priv_deallocate(to_raw_pointer(chain.pop_front()));
      }
   }
};

}  //namespace ipcdetail {
}  //namespace interprocess {
}  //namespace boost {

#include <boost/interprocess/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTERPROCESS_DETAIL_MEM_ALGO_COMMON_HPP
