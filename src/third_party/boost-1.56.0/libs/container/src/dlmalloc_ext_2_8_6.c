//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////


#define BOOST_CONTAINER_SOURCE
#include <boost/container/detail/alloc_lib.h>

#include "errno.h"   //dlmalloc bug EINVAL is used in posix_memalign without checking LACKS_ERRNO_H
#include "limits.h"  //CHAR_BIT
#ifdef BOOST_CONTAINER_DLMALLOC_FOOTERS
#define FOOTERS      1
#endif
#define USE_LOCKS    1
#define MSPACES      1
#define NO_MALLINFO  0

#if !defined(NDEBUG)
   #if !defined(DEBUG)
      #define DEBUG 1
      #define DL_DEBUG_DEFINED
   #endif
#endif

#define USE_DL_PREFIX
#define FORCEINLINE
#include "dlmalloc_2_8_6.c"

#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable : 4127)
#pragma warning (disable : 4267)
#pragma warning (disable : 4127)
#pragma warning (disable : 4702)
#pragma warning (disable : 4390) /*empty controlled statement found; is this the intent?*/
#pragma warning (disable : 4251 4231 4660) /*dll warnings*/
#endif

#define DL_SIZE_IMPL(p) (chunksize(mem2chunk(p)) - overhead_for(mem2chunk(p)))

static size_t s_allocated_memory;

///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
//
//         SLIGHTLY MODIFIED DLMALLOC FUNCTIONS
//
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////

//This function is equal to mspace_free
//replacing PREACTION with 0 and POSTACTION with nothing
static void mspace_free_lockless(mspace msp, void* mem)
{
  if (mem != 0) {
    mchunkptr p  = mem2chunk(mem);
#if FOOTERS
    mstate fm = get_mstate_for(p);
    msp = msp; /* placate people compiling -Wunused */
#else /* FOOTERS */
    mstate fm = (mstate)msp;
#endif /* FOOTERS */
    if (!ok_magic(fm)) {
      USAGE_ERROR_ACTION(fm, p);
      return;
    }
    if (!0){//PREACTION(fm)) {
      check_inuse_chunk(fm, p);
      if (RTCHECK(ok_address(fm, p) && ok_inuse(p))) {
        size_t psize = chunksize(p);
        mchunkptr next = chunk_plus_offset(p, psize);
        s_allocated_memory -= psize;
        if (!pinuse(p)) {
          size_t prevsize = p->prev_foot;
          if (is_mmapped(p)) {
            psize += prevsize + MMAP_FOOT_PAD;
            if (CALL_MUNMAP((char*)p - prevsize, psize) == 0)
              fm->footprint -= psize;
            goto postaction;
          }
          else {
            mchunkptr prev = chunk_minus_offset(p, prevsize);
            psize += prevsize;
            p = prev;
            if (RTCHECK(ok_address(fm, prev))) { /* consolidate backward */
              if (p != fm->dv) {
                unlink_chunk(fm, p, prevsize);
              }
              else if ((next->head & INUSE_BITS) == INUSE_BITS) {
                fm->dvsize = psize;
                set_free_with_pinuse(p, psize, next);
                goto postaction;
              }
            }
            else
              goto erroraction;
          }
        }

        if (RTCHECK(ok_next(p, next) && ok_pinuse(next))) {
          if (!cinuse(next)) {  /* consolidate forward */
            if (next == fm->top) {
              size_t tsize = fm->topsize += psize;
              fm->top = p;
              p->head = tsize | PINUSE_BIT;
              if (p == fm->dv) {
                fm->dv = 0;
                fm->dvsize = 0;
              }
              if (should_trim(fm, tsize))
                sys_trim(fm, 0);
              goto postaction;
            }
            else if (next == fm->dv) {
              size_t dsize = fm->dvsize += psize;
              fm->dv = p;
              set_size_and_pinuse_of_free_chunk(p, dsize);
              goto postaction;
            }
            else {
              size_t nsize = chunksize(next);
              psize += nsize;
              unlink_chunk(fm, next, nsize);
              set_size_and_pinuse_of_free_chunk(p, psize);
              if (p == fm->dv) {
                fm->dvsize = psize;
                goto postaction;
              }
            }
          }
          else
            set_free_with_pinuse(p, psize, next);

          if (is_small(psize)) {
            insert_small_chunk(fm, p, psize);
            check_free_chunk(fm, p);
          }
          else {
            tchunkptr tp = (tchunkptr)p;
            insert_large_chunk(fm, tp, psize);
            check_free_chunk(fm, p);
            if (--fm->release_checks == 0)
              release_unused_segments(fm);
          }
          goto postaction;
        }
      }
    erroraction:
      USAGE_ERROR_ACTION(fm, p);
    postaction:
      ;//POSTACTION(fm);
    }
  }
}

//This function is equal to mspace_malloc
//replacing PREACTION with 0 and POSTACTION with nothing
void* mspace_malloc_lockless(mspace msp, size_t bytes)
{
  mstate ms = (mstate)msp;
  if (!ok_magic(ms)) {
    USAGE_ERROR_ACTION(ms,ms);
    return 0;
  }
    if (!0){//PREACTION(ms)) {
    void* mem;
    size_t nb;
    if (bytes <= MAX_SMALL_REQUEST) {
      bindex_t idx;
      binmap_t smallbits;
      nb = (bytes < MIN_REQUEST)? MIN_CHUNK_SIZE : pad_request(bytes);
      idx = small_index(nb);
      smallbits = ms->smallmap >> idx;

      if ((smallbits & 0x3U) != 0) { /* Remainderless fit to a smallbin. */
        mchunkptr b, p;
        idx += ~smallbits & 1;       /* Uses next bin if idx empty */
        b = smallbin_at(ms, idx);
        p = b->fd;
        assert(chunksize(p) == small_index2size(idx));
        unlink_first_small_chunk(ms, b, p, idx);
        set_inuse_and_pinuse(ms, p, small_index2size(idx));
        mem = chunk2mem(p);
        check_malloced_chunk(ms, mem, nb);
        goto postaction;
      }

      else if (nb > ms->dvsize) {
        if (smallbits != 0) { /* Use chunk in next nonempty smallbin */
          mchunkptr b, p, r;
          size_t rsize;
          bindex_t i;
          binmap_t leftbits = (smallbits << idx) & left_bits(idx2bit(idx));
          binmap_t leastbit = least_bit(leftbits);
          compute_bit2idx(leastbit, i);
          b = smallbin_at(ms, i);
          p = b->fd;
          assert(chunksize(p) == small_index2size(i));
          unlink_first_small_chunk(ms, b, p, i);
          rsize = small_index2size(i) - nb;
          /* Fit here cannot be remainderless if 4byte sizes */
          if (SIZE_T_SIZE != 4 && rsize < MIN_CHUNK_SIZE)
            set_inuse_and_pinuse(ms, p, small_index2size(i));
          else {
            set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
            r = chunk_plus_offset(p, nb);
            set_size_and_pinuse_of_free_chunk(r, rsize);
            replace_dv(ms, r, rsize);
          }
          mem = chunk2mem(p);
          check_malloced_chunk(ms, mem, nb);
          goto postaction;
        }

        else if (ms->treemap != 0 && (mem = tmalloc_small(ms, nb)) != 0) {
          check_malloced_chunk(ms, mem, nb);
          goto postaction;
        }
      }
    }
    else if (bytes >= MAX_REQUEST)
      nb = MAX_SIZE_T; /* Too big to allocate. Force failure (in sys alloc) */
    else {
      nb = pad_request(bytes);
      if (ms->treemap != 0 && (mem = tmalloc_large(ms, nb)) != 0) {
        check_malloced_chunk(ms, mem, nb);
        goto postaction;
      }
    }

    if (nb <= ms->dvsize) {
      size_t rsize = ms->dvsize - nb;
      mchunkptr p = ms->dv;
      if (rsize >= MIN_CHUNK_SIZE) { /* split dv */
        mchunkptr r = ms->dv = chunk_plus_offset(p, nb);
        ms->dvsize = rsize;
        set_size_and_pinuse_of_free_chunk(r, rsize);
        set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
      }
      else { /* exhaust dv */
        size_t dvs = ms->dvsize;
        ms->dvsize = 0;
        ms->dv = 0;
        set_inuse_and_pinuse(ms, p, dvs);
      }
      mem = chunk2mem(p);
      check_malloced_chunk(ms, mem, nb);
      goto postaction;
    }

    else if (nb < ms->topsize) { /* Split top */
      size_t rsize = ms->topsize -= nb;
      mchunkptr p = ms->top;
      mchunkptr r = ms->top = chunk_plus_offset(p, nb);
      r->head = rsize | PINUSE_BIT;
      set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
      mem = chunk2mem(p);
      check_top_chunk(ms, ms->top);
      check_malloced_chunk(ms, mem, nb);
      goto postaction;
    }

    mem = sys_alloc(ms, nb);

  postaction:
      ;//POSTACTION(ms);
    return mem;
  }

  return 0;
}

//This function is equal to try_realloc_chunk but handling
//minimum and desired bytes
static mchunkptr try_realloc_chunk_with_min(mstate m, mchunkptr p, size_t min_nb, size_t des_nb, int can_move)
{
  mchunkptr newp = 0;
  size_t oldsize = chunksize(p);
  mchunkptr next = chunk_plus_offset(p, oldsize);
  if (RTCHECK(ok_address(m, p) && ok_inuse(p) &&
              ok_next(p, next) && ok_pinuse(next))) {
    if (is_mmapped(p)) {
      newp = mmap_resize(m, p, des_nb, can_move);
      if(!newp)   //mmap does not return how many bytes we could reallocate, so go the minimum
         newp = mmap_resize(m, p, min_nb, can_move);
    }
    else if (oldsize >= min_nb) {             /* already big enough */
      size_t nb = oldsize >= des_nb ? des_nb : oldsize;
      size_t rsize = oldsize - nb;
      if (rsize >= MIN_CHUNK_SIZE) {      /* split off remainder */
        mchunkptr r = chunk_plus_offset(p, nb);
        set_inuse(m, p, nb);
        set_inuse(m, r, rsize);
        dispose_chunk(m, r, rsize);
      }
      newp = p;
    }
    else if (next == m->top) {  /* extend into top */
      if (oldsize + m->topsize > min_nb) {
        size_t nb = (oldsize + m->topsize) > des_nb ? des_nb : (oldsize + m->topsize - MALLOC_ALIGNMENT);
        size_t newsize = oldsize + m->topsize;
        size_t newtopsize = newsize - nb;
        mchunkptr newtop = chunk_plus_offset(p, nb);
        set_inuse(m, p, nb);
        newtop->head = newtopsize |PINUSE_BIT;
        m->top = newtop;
        m->topsize = newtopsize;
        newp = p;
      }
    }
    else if (next == m->dv) { /* extend into dv */
      size_t dvs = m->dvsize;
      if (oldsize + dvs >= min_nb) {
        size_t nb = (oldsize + dvs) >= des_nb ? des_nb : (oldsize + dvs);
        size_t dsize = oldsize + dvs - nb;
        if (dsize >= MIN_CHUNK_SIZE) {
          mchunkptr r = chunk_plus_offset(p, nb);
          mchunkptr n = chunk_plus_offset(r, dsize);
          set_inuse(m, p, nb);
          set_size_and_pinuse_of_free_chunk(r, dsize);
          clear_pinuse(n);
          m->dvsize = dsize;
          m->dv = r;
        }
        else { /* exhaust dv */
          size_t newsize = oldsize + dvs;
          set_inuse(m, p, newsize);
          m->dvsize = 0;
          m->dv = 0;
        }
        newp = p;
      }
    }
    else if (!cinuse(next)) { /* extend into next free chunk */
      size_t nextsize = chunksize(next);
      if (oldsize + nextsize >= min_nb) {
        size_t nb = (oldsize + nextsize) >= des_nb ? des_nb : (oldsize + nextsize);
        size_t rsize = oldsize + nextsize - nb;
        unlink_chunk(m, next, nextsize);
        if (rsize < MIN_CHUNK_SIZE) {
          size_t newsize = oldsize + nextsize;
          set_inuse(m, p, newsize);
        }
        else {
          mchunkptr r = chunk_plus_offset(p, nb);
          set_inuse(m, p, nb);
          set_inuse(m, r, rsize);
          dispose_chunk(m, r, rsize);
        }
        newp = p;
      }
    }
  }
  else {
    USAGE_ERROR_ACTION(m, chunk2mem(p));
  }
  return newp;
}

#define BOOST_ALLOC_PLUS_MEMCHAIN_MEM_JUMP_NEXT(THISMEM, NEXTMEM) \
   *((void**)(THISMEM)) = *((void**)((NEXTMEM)))

//This function is based on internal_bulk_free
//replacing iteration over array[] with boost_cont_memchain.
//Instead of returning the unallocated nodes, returns a chain of non-deallocated nodes.
//After forward merging, backwards merging is also tried
static void internal_multialloc_free(mstate m, boost_cont_memchain *pchain)
{
#if FOOTERS
  boost_cont_memchain ret_chain;
  BOOST_CONTAINER_MEMCHAIN_INIT(&ret_chain);
#endif
  if (!PREACTION(m)) {
    boost_cont_memchain_it a_it = BOOST_CONTAINER_MEMCHAIN_BEGIN_IT(pchain);
    while(!BOOST_CONTAINER_MEMCHAIN_IS_END_IT(pchain, a_it)) { /* Iterate though all memory holded by the chain */
      void* a_mem = BOOST_CONTAINER_MEMIT_ADDR(a_it);
      mchunkptr a_p = mem2chunk(a_mem);
      size_t psize = chunksize(a_p);
#if FOOTERS
      if (get_mstate_for(a_p) != m) {
         BOOST_CONTAINER_MEMIT_NEXT(a_it);
         BOOST_CONTAINER_MEMCHAIN_PUSH_BACK(&ret_chain, a_mem);
         continue;
      }
#endif
      check_inuse_chunk(m, a_p);
      if (RTCHECK(ok_address(m, a_p) && ok_inuse(a_p))) {
         while(1) { /* Internal loop to speed up forward and backward merging (avoids some redundant checks) */
            boost_cont_memchain_it b_it = a_it;
            BOOST_CONTAINER_MEMIT_NEXT(b_it);
            if(!BOOST_CONTAINER_MEMCHAIN_IS_END_IT(pchain, b_it)){
               void *b_mem   = BOOST_CONTAINER_MEMIT_ADDR(b_it);
               mchunkptr b_p = mem2chunk(b_mem);
               if (b_p == next_chunk(a_p)) { /* b chunk is contiguous and next so b's size can be added to a */
                  psize += chunksize(b_p);
                  set_inuse(m, a_p, psize);
                  BOOST_ALLOC_PLUS_MEMCHAIN_MEM_JUMP_NEXT(a_mem, b_mem);
                  continue;
               }
               if(RTCHECK(ok_address(m, b_p) && ok_inuse(b_p))){
                  /* b chunk is contiguous and previous so a's size can be added to b */
                  if(a_p == next_chunk(b_p)) {
                     psize += chunksize(b_p);
                     set_inuse(m, b_p, psize);
                     a_it = b_it;
                     a_p = b_p;
                     a_mem = b_mem;
                     continue;
                  }
               }
            }
            /* Normal deallocation starts again in the outer loop */
            a_it = b_it;
            s_allocated_memory -= psize;
            dispose_chunk(m, a_p, psize);
            break;
         }
       }
       else {
         CORRUPTION_ERROR_ACTION(m);
         break;
       }
    }
    if (should_trim(m, m->topsize))
      sys_trim(m, 0);
    POSTACTION(m);
  }
#if FOOTERS
  {
   boost_cont_memchain_it last_pchain = BOOST_CONTAINER_MEMCHAIN_LAST_IT(pchain);
   BOOST_CONTAINER_MEMCHAIN_INIT(pchain);
   BOOST_CONTAINER_MEMCHAIN_INCORPORATE_AFTER
         (pchain
         , last_pchain
         , BOOST_CONTAINER_MEMCHAIN_FIRSTMEM(&ret_chain)
         , BOOST_CONTAINER_MEMCHAIN_LASTMEM(&ret_chain)
         , BOOST_CONTAINER_MEMCHAIN_SIZE(&ret_chain)
         );
   }
#endif
}

///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
//
//         NEW FUNCTIONS BASED ON DLMALLOC INTERNALS
//
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////

#define GET_TRUNCATED_SIZE(ORIG_SIZE, ROUNDTO)     ((ORIG_SIZE)/(ROUNDTO)*(ROUNDTO))
#define GET_ROUNDED_SIZE(ORIG_SIZE, ROUNDTO)       ((((ORIG_SIZE)-1)/(ROUNDTO)+1)*(ROUNDTO))
#define GET_TRUNCATED_PO2_SIZE(ORIG_SIZE, ROUNDTO) ((ORIG_SIZE) & (~(ROUNDTO-1)))
#define GET_ROUNDED_PO2_SIZE(ORIG_SIZE, ROUNDTO)   (((ORIG_SIZE - 1) & (~(ROUNDTO-1))) + ROUNDTO)

/* Greatest common divisor and least common multiple
   gcd is an algorithm that calculates the greatest common divisor of two
   integers, using Euclid's algorithm.

   Pre: A > 0 && B > 0
   Recommended: A > B*/
#define CALCULATE_GCD(A, B, OUT)\
{\
   size_t a = A;\
   size_t b = B;\
   do\
   {\
      size_t tmp = b;\
      b = a % b;\
      a = tmp;\
   } while (b != 0);\
\
   OUT = a;\
}

/* lcm is an algorithm that calculates the least common multiple of two
   integers.

   Pre: A > 0 && B > 0
   Recommended: A > B*/
#define CALCULATE_LCM(A, B, OUT)\
{\
   CALCULATE_GCD(A, B, OUT);\
   OUT = (A / OUT)*B;\
}

static int calculate_lcm_and_needs_backwards_lcmed
   (size_t backwards_multiple, size_t received_size, size_t size_to_achieve,
    size_t *plcm, size_t *pneeds_backwards_lcmed)
{
   /* Now calculate lcm */
   size_t max = backwards_multiple;
   size_t min = MALLOC_ALIGNMENT;
   size_t needs_backwards;
   size_t needs_backwards_lcmed;
   size_t lcm;
   size_t current_forward;
   /*Swap if necessary*/
   if(max < min){
      size_t tmp = min;
      min = max;
      max = tmp;
   }
   /*Check if it's power of two*/
   if((backwards_multiple & (backwards_multiple-1)) == 0){
      if(0 != (size_to_achieve & ((backwards_multiple-1)))){
         USAGE_ERROR_ACTION(m, oldp);
         return 0;
      }

      lcm = max;
      /*If we want to use minbytes data to get a buffer between maxbytes
      and minbytes if maxbytes can't be achieved, calculate the
      biggest of all possibilities*/
      current_forward = GET_TRUNCATED_PO2_SIZE(received_size, backwards_multiple);
      needs_backwards = size_to_achieve - current_forward;
      assert((needs_backwards % backwards_multiple) == 0);
      needs_backwards_lcmed = GET_ROUNDED_PO2_SIZE(needs_backwards, lcm);
      *plcm = lcm;
      *pneeds_backwards_lcmed = needs_backwards_lcmed;
      return 1;
   }
   /*Check if it's multiple of alignment*/
   else if((backwards_multiple & (MALLOC_ALIGNMENT - 1u)) == 0){
      lcm = backwards_multiple;
      current_forward = GET_TRUNCATED_SIZE(received_size, backwards_multiple);
      //No need to round needs_backwards because backwards_multiple == lcm
      needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
      assert((needs_backwards_lcmed & (MALLOC_ALIGNMENT - 1u)) == 0);
      *plcm = lcm;
      *pneeds_backwards_lcmed = needs_backwards_lcmed;
      return 1;
   }
   /*Check if it's multiple of the half of the alignmment*/
   else if((backwards_multiple & ((MALLOC_ALIGNMENT/2u) - 1u)) == 0){
      lcm = backwards_multiple*2u;
      current_forward = GET_TRUNCATED_SIZE(received_size, backwards_multiple);
      needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
      if(0 != (needs_backwards_lcmed & (MALLOC_ALIGNMENT-1)))
      //while(0 != (needs_backwards_lcmed & (MALLOC_ALIGNMENT-1)))
         needs_backwards_lcmed += backwards_multiple;
      assert((needs_backwards_lcmed % lcm) == 0);
      *plcm = lcm;
      *pneeds_backwards_lcmed = needs_backwards_lcmed;
      return 1;
   }
   /*Check if it's multiple of the quarter of the alignmment*/
   else if((backwards_multiple & ((MALLOC_ALIGNMENT/4u) - 1u)) == 0){
      size_t remainder;
      lcm = backwards_multiple*4u;
      current_forward = GET_TRUNCATED_SIZE(received_size, backwards_multiple);
      needs_backwards_lcmed = needs_backwards = size_to_achieve - current_forward;
      //while(0 != (needs_backwards_lcmed & (MALLOC_ALIGNMENT-1)))
         //needs_backwards_lcmed += backwards_multiple;
      if(0 != (remainder = ((needs_backwards_lcmed & (MALLOC_ALIGNMENT-1))>>(MALLOC_ALIGNMENT/8u)))){
         if(backwards_multiple & MALLOC_ALIGNMENT/2u){
            needs_backwards_lcmed += (remainder)*backwards_multiple;
         }
         else{
            needs_backwards_lcmed += (4-remainder)*backwards_multiple;
         }
      }
      assert((needs_backwards_lcmed % lcm) == 0);
      *plcm = lcm;
      *pneeds_backwards_lcmed = needs_backwards_lcmed;
      return 1;
   }
   else{
      CALCULATE_LCM(max, min, lcm);
      /*If we want to use minbytes data to get a buffer between maxbytes
      and minbytes if maxbytes can't be achieved, calculate the
      biggest of all possibilities*/
      current_forward = GET_TRUNCATED_SIZE(received_size, backwards_multiple);
      needs_backwards = size_to_achieve - current_forward;
      assert((needs_backwards % backwards_multiple) == 0);
      needs_backwards_lcmed = GET_ROUNDED_SIZE(needs_backwards, lcm);
      *plcm = lcm;
      *pneeds_backwards_lcmed = needs_backwards_lcmed;
      return 1;
   }
}

static void *internal_grow_both_sides
                         (mstate m
                         ,allocation_type command
                         ,void *oldmem
                         ,size_t minbytes
                         ,size_t maxbytes
                         ,size_t *received_size
                         ,size_t backwards_multiple
                         ,int only_preferred_backwards)
{
   mchunkptr oldp = mem2chunk(oldmem);
   size_t oldsize = chunksize(oldp);
   *received_size = oldsize - overhead_for(oldp);
   if(minbytes <= *received_size)
      return oldmem;

   if (RTCHECK(ok_address(m, oldp) && ok_inuse(oldp))) {
      if(command & BOOST_CONTAINER_EXPAND_FWD){
         if(try_realloc_chunk_with_min(m, oldp, request2size(minbytes), request2size(maxbytes), 0)){
            check_inuse_chunk(m, oldp);
            *received_size = DL_SIZE_IMPL(oldmem);
            s_allocated_memory += chunksize(oldp) - oldsize;
            return oldmem;
         }
      }
      else{
         *received_size = DL_SIZE_IMPL(oldmem);
         if(*received_size >= maxbytes)
            return oldmem;
      }
/*
      Should we check this?
      if(backwards_multiple &&
         (0 != (minbytes % backwards_multiple) &&
          0 != (maxbytes % backwards_multiple)) ){
         USAGE_ERROR_ACTION(m, oldp);
         return 0;
      }
*/
      /* We reach here only if forward expansion fails */
      if(!(command & BOOST_CONTAINER_EXPAND_BWD) || pinuse(oldp)){
         return 0;
      }
      {
         size_t prevsize = oldp->prev_foot;
         if ((prevsize & USE_MMAP_BIT) != 0){
            /*Return failure the previous chunk was mmapped.
              mremap does not allow expanding to a fixed address (MREMAP_MAYMOVE) without
              copying (MREMAP_MAYMOVE must be also set).*/
            return 0;
         }
         else {
            mchunkptr prev = chunk_minus_offset(oldp, prevsize);
            size_t dsize = oldsize + prevsize;
            size_t needs_backwards_lcmed;
            size_t lcm;

            /* Let's calculate the number of extra bytes of data before the current
            block's begin. The value is a multiple of backwards_multiple
            and the alignment*/
            if(!calculate_lcm_and_needs_backwards_lcmed
               ( backwards_multiple, *received_size
               , only_preferred_backwards ? maxbytes : minbytes
               , &lcm, &needs_backwards_lcmed)
               || !RTCHECK(ok_address(m, prev))){
               USAGE_ERROR_ACTION(m, oldp);
               return 0;
            }
            /* Check if previous block has enough size */
            else if(prevsize < needs_backwards_lcmed){
               /* preferred size? */
               return 0;
            }
            /* Now take all next space. This must succeed, as we've previously calculated the correct size */
            if(command & BOOST_CONTAINER_EXPAND_FWD){
               if(!try_realloc_chunk_with_min(m, oldp, request2size(*received_size), request2size(*received_size), 0)){
                  assert(0);
               }
               check_inuse_chunk(m, oldp);
               *received_size = DL_SIZE_IMPL(oldmem);
               s_allocated_memory += chunksize(oldp) - oldsize;
               oldsize = chunksize(oldp);
               dsize = oldsize + prevsize;
            }
            /* We need a minimum size to split the previous one */
            if(prevsize >= (needs_backwards_lcmed + MIN_CHUNK_SIZE)){
               mchunkptr r  = chunk_minus_offset(oldp, needs_backwards_lcmed);
               size_t rsize = oldsize + needs_backwards_lcmed;
               size_t newprevsize = dsize - rsize;
               int prev_was_dv = prev == m->dv;

               assert(newprevsize >= MIN_CHUNK_SIZE);

               if (prev_was_dv) {
                  m->dvsize = newprevsize;
               }
               else{/* if ((next->head & INUSE_BITS) == INUSE_BITS) { */
                  unlink_chunk(m, prev, prevsize);
                  insert_chunk(m, prev, newprevsize);
               }

               set_size_and_pinuse_of_free_chunk(prev, newprevsize);
               clear_pinuse(r);
               set_inuse(m, r, rsize);
               check_malloced_chunk(m, chunk2mem(r), rsize);
               *received_size = chunksize(r) - overhead_for(r);
               s_allocated_memory += chunksize(r) - oldsize;
               return chunk2mem(r);
            }
            /* Check if there is no place to create a new block and
               the whole new block is multiple of the backwards expansion multiple */
            else if(prevsize >= needs_backwards_lcmed && !(prevsize % lcm)) {
               /* Just merge the whole previous block */
               /* prevsize is multiple of lcm (and backwards_multiple)*/
               *received_size  += prevsize;

               if (prev != m->dv) {
                  unlink_chunk(m, prev, prevsize);
               }
               else{
                  m->dvsize = 0;
                  m->dv     = 0;
               }
               set_inuse(m, prev, dsize);
               check_malloced_chunk(m, chunk2mem(prev), dsize);
               s_allocated_memory += chunksize(prev) - oldsize;
               return chunk2mem(prev);
            }
            else{
               /* Previous block was big enough but there is no room
                  to create an empty block and taking the whole block does
                  not fulfill alignment requirements */
               return 0;
            }
         }
      }
   }
   else{
      USAGE_ERROR_ACTION(m, oldmem);
      return 0;
   }
   return 0;
}

/* This is similar to mmap_resize but:
   * Only to shrink
   * It takes min and max sizes
   * Takes additional 'do_commit' argument to obtain the final
     size before doing the real shrink operation.
*/
static int internal_mmap_shrink_in_place(mstate m, mchunkptr oldp, size_t nbmin, size_t nbmax, size_t *received_size, int do_commit)
{
  size_t oldsize = chunksize(oldp);
  *received_size = oldsize;
  #if HAVE_MREMAP
  if (is_small(nbmax)) /* Can't shrink mmap regions below small size */
    return 0;
  {
   size_t effective_min = nbmin > MIN_LARGE_SIZE ? nbmin : MIN_LARGE_SIZE;
   /* Keep old chunk if big enough but not too big */
   if (oldsize >= effective_min + SIZE_T_SIZE &&
         (oldsize - effective_min) <= (mparams.granularity << 1))
      return 0;
   /* Now calculate new sizes */
   {
      size_t offset = oldp->prev_foot;
      size_t oldmmsize = oldsize + offset + MMAP_FOOT_PAD;
      size_t newmmsize = mmap_align(effective_min + SIX_SIZE_T_SIZES + CHUNK_ALIGN_MASK);
      *received_size = newmmsize;
      if(!do_commit){
         const int flags = 0; /* placate people compiling -Wunused */
         char* cp = (char*)CALL_MREMAP((char*)oldp - offset,
                                       oldmmsize, newmmsize, flags);
         /*This must always succeed */
         if(!cp){
            USAGE_ERROR_ACTION(m, m);
            return 0;
         }
         {
         mchunkptr newp = (mchunkptr)(cp + offset);
         size_t psize = newmmsize - offset - MMAP_FOOT_PAD;
         newp->head = psize;
         mark_inuse_foot(m, newp, psize);
         chunk_plus_offset(newp, psize)->head = FENCEPOST_HEAD;
         chunk_plus_offset(newp, psize+SIZE_T_SIZE)->head = 0;

         if (cp < m->least_addr)
            m->least_addr = cp;
         if ((m->footprint += newmmsize - oldmmsize) > m->max_footprint)
            m->max_footprint = m->footprint;
         check_mmapped_chunk(m, newp);
         }
      }
    }
    return 1;
  }
  #else  //#if HAVE_MREMAP
  (void)m;
  (void)oldp;
  (void)nbmin;
  (void)nbmax;
  (void)received_size;
  (void)do_commit;
  return 0;
  #endif //#if HAVE_MREMAP
}

static int internal_shrink(mstate m, void* oldmem, size_t minbytes, size_t maxbytes, size_t *received_size, int do_commit)
{
   *received_size = chunksize(mem2chunk(oldmem)) - overhead_for(mem2chunk(oldmem));
   if (minbytes >= MAX_REQUEST || maxbytes >= MAX_REQUEST) {
      MALLOC_FAILURE_ACTION;
      return 0;
   }
   else if(minbytes < MIN_REQUEST){
      minbytes = MIN_REQUEST;
   }
   if (minbytes > maxbytes) {
      return 0;
   }

   {
      mchunkptr oldp = mem2chunk(oldmem);
      size_t oldsize = chunksize(oldp);
      mchunkptr next = chunk_plus_offset(oldp, oldsize);
      void* extra = 0;

      /* Try to either shrink or extend into top. Else malloc-copy-free*/
      if (RTCHECK(ok_address(m, oldp) && ok_inuse(oldp) &&
                  ok_next(oldp, next) && ok_pinuse(next))) {
         size_t nbmin = request2size(minbytes);
         size_t nbmax = request2size(maxbytes);

         if (nbmin > oldsize){
            /* Return error if old size is too small */
         }
         else if (is_mmapped(oldp)){
            return internal_mmap_shrink_in_place(m, oldp, nbmin, nbmax, received_size, do_commit);
         }
         else{ // nbmin <= oldsize /* already big enough*/
            size_t nb = nbmin;
            size_t rsize = oldsize - nb;
            if (rsize >= MIN_CHUNK_SIZE) {
               if(do_commit){
                  mchunkptr remainder = chunk_plus_offset(oldp, nb);
                  set_inuse(m, oldp, nb);
                  set_inuse(m, remainder, rsize);
                  extra = chunk2mem(remainder);
               }
               *received_size = nb - overhead_for(oldp);
               if(!do_commit)
                  return 1;
            }
         }
      }
      else {
         USAGE_ERROR_ACTION(m, oldmem);
         return 0;
      }

      if (extra != 0 && do_commit) {
         mspace_free_lockless(m, extra);
         check_inuse_chunk(m, oldp);
         return 1;
      }
      else {
         return 0;
      }
   }
}


#define INTERNAL_MULTIALLOC_DEFAULT_CONTIGUOUS_MEM 4096

#define SQRT_MAX_SIZE_T           (((size_t)-1)>>(sizeof(size_t)*CHAR_BIT/2))

static int internal_node_multialloc
   (mstate m, size_t n_elements, size_t element_size, size_t contiguous_elements, boost_cont_memchain *pchain) {
   void*     mem;            /* malloced aggregate space */
   mchunkptr p;              /* corresponding chunk */
   size_t    remainder_size; /* remaining bytes while splitting */
   flag_t    was_enabled;    /* to disable mmap */
   size_t    elements_per_segment = 0;
   size_t    element_req_size = request2size(element_size);
   boost_cont_memchain_it prev_last_it = BOOST_CONTAINER_MEMCHAIN_LAST_IT(pchain);

   /*Error if wrong element_size parameter */
   if( !element_size ||
      /*OR Error if n_elements less thatn contiguous_elements */
      ((contiguous_elements + 1) > (DL_MULTIALLOC_DEFAULT_CONTIGUOUS + 1) && n_elements < contiguous_elements) ||
      /* OR Error if integer overflow */
      (SQRT_MAX_SIZE_T < (element_req_size | contiguous_elements) &&
         (MAX_SIZE_T/element_req_size) < contiguous_elements)){
      return 0;
   }
   switch(contiguous_elements){
      case DL_MULTIALLOC_DEFAULT_CONTIGUOUS:
      {
         /* Default contiguous, just check that we can store at least one element */
         elements_per_segment = INTERNAL_MULTIALLOC_DEFAULT_CONTIGUOUS_MEM/element_req_size;
         elements_per_segment += (size_t)(!elements_per_segment);
      }
      break;
      case DL_MULTIALLOC_ALL_CONTIGUOUS:
         /* All elements should be allocated in a single call */
         elements_per_segment = n_elements;
      break;
      default:
         /* Allocate in chunks of "contiguous_elements" */
         elements_per_segment = contiguous_elements;
   }

   {
      size_t    i;
      size_t next_i;
      /*
         Allocate the aggregate chunk.  First disable direct-mmapping so
         malloc won't use it, since we would not be able to later
         free/realloc space internal to a segregated mmap region.
      */
      was_enabled = use_mmap(m);
      disable_mmap(m);
      for(i = 0; i != n_elements; i = next_i)
      {
         size_t accum_size;
         size_t n_elements_left = n_elements - i;
         next_i = i + ((n_elements_left < elements_per_segment) ? n_elements_left : elements_per_segment);
         accum_size = element_req_size*(next_i - i);

         mem = mspace_malloc_lockless(m, accum_size - CHUNK_OVERHEAD);
         if (mem == 0){
            BOOST_CONTAINER_MEMIT_NEXT(prev_last_it);
            while(i--){
               void *addr = BOOST_CONTAINER_MEMIT_ADDR(prev_last_it);
               BOOST_CONTAINER_MEMIT_NEXT(prev_last_it);
               mspace_free_lockless(m, addr);
            }
            if (was_enabled)
               enable_mmap(m);
            return 0;
         }
         p = mem2chunk(mem);
         remainder_size = chunksize(p);
         s_allocated_memory += remainder_size;

         assert(!is_mmapped(p));
         {  /* split out elements */
            void *mem_orig = mem;
            boost_cont_memchain_it last_it = BOOST_CONTAINER_MEMCHAIN_LAST_IT(pchain);
            size_t num_elements = next_i-i;

            size_t num_loops = num_elements - 1;
            remainder_size -= element_req_size*num_loops;
            while(num_loops--){
               void **mem_prev = ((void**)mem);
               set_size_and_pinuse_of_inuse_chunk(m, p, element_req_size);
               p = chunk_plus_offset(p, element_req_size);
               mem = chunk2mem(p);
               *mem_prev = mem;
            }
            set_size_and_pinuse_of_inuse_chunk(m, p, remainder_size);
            BOOST_CONTAINER_MEMCHAIN_INCORPORATE_AFTER(pchain, last_it, mem_orig, mem, num_elements);
         }
      }
      if (was_enabled)
         enable_mmap(m);
   }
   return 1;
}

static int internal_multialloc_arrays
   (mstate m, size_t n_elements, const size_t* sizes, size_t element_size, size_t contiguous_elements, boost_cont_memchain *pchain) {
   void*     mem;            /* malloced aggregate space */
   mchunkptr p;              /* corresponding chunk */
   size_t    remainder_size; /* remaining bytes while splitting */
   flag_t    was_enabled;    /* to disable mmap */
   size_t    size;
   size_t boost_cont_multialloc_segmented_malloc_size;
   size_t max_size;

   /* Check overflow */
   if(!element_size){
      return 0;
   }
   max_size = MAX_REQUEST/element_size;
   /* Different sizes*/
   switch(contiguous_elements){
      case DL_MULTIALLOC_DEFAULT_CONTIGUOUS:
         /* Use default contiguous mem */
         boost_cont_multialloc_segmented_malloc_size = INTERNAL_MULTIALLOC_DEFAULT_CONTIGUOUS_MEM;
      break;
      case DL_MULTIALLOC_ALL_CONTIGUOUS:
         boost_cont_multialloc_segmented_malloc_size = MAX_REQUEST + CHUNK_OVERHEAD;
      break;
      default:
         if(max_size < contiguous_elements){
            return 0;
         }
         else{
            /* The suggested buffer is just the the element count by the size */
            boost_cont_multialloc_segmented_malloc_size = element_size*contiguous_elements;
         }
   }

   {
      size_t    i;
      size_t next_i;
      /*
         Allocate the aggregate chunk.  First disable direct-mmapping so
         malloc won't use it, since we would not be able to later
         free/realloc space internal to a segregated mmap region.
      */
      was_enabled = use_mmap(m);
      disable_mmap(m);
      for(i = 0, next_i = 0; i != n_elements; i = next_i)
      {
         int error = 0;
         size_t accum_size;
         for(accum_size = 0; next_i != n_elements; ++next_i){
            size_t cur_array_size   = sizes[next_i];
            if(max_size < cur_array_size){
               error = 1;
               break;
            }
            else{
               size_t reqsize = request2size(cur_array_size*element_size);
               if(((boost_cont_multialloc_segmented_malloc_size - CHUNK_OVERHEAD) - accum_size) < reqsize){
                  if(!accum_size){
                     accum_size += reqsize;
                     ++next_i;
                  }
                  break;
               }
               accum_size += reqsize;
            }
         }

         mem = error ? 0 : mspace_malloc_lockless(m, accum_size - CHUNK_OVERHEAD);
         if (mem == 0){
            boost_cont_memchain_it it = BOOST_CONTAINER_MEMCHAIN_BEGIN_IT(pchain);
            while(i--){
               void *addr = BOOST_CONTAINER_MEMIT_ADDR(it);
               BOOST_CONTAINER_MEMIT_NEXT(it);
               mspace_free_lockless(m, addr);
            }
            if (was_enabled)
               enable_mmap(m);
            return 0;
         }
         p = mem2chunk(mem);
         remainder_size = chunksize(p);
         s_allocated_memory += remainder_size;

         assert(!is_mmapped(p));

         {  /* split out elements */
            void *mem_orig = mem;
            boost_cont_memchain_it last_it = BOOST_CONTAINER_MEMCHAIN_LAST_IT(pchain);
            size_t num_elements = next_i-i;

            for(++i; i != next_i; ++i) {
               void **mem_prev = ((void**)mem);
               size = request2size(sizes[i]*element_size);
               remainder_size -= size;
               set_size_and_pinuse_of_inuse_chunk(m, p, size);
               p = chunk_plus_offset(p, size);
               mem = chunk2mem(p);
               *mem_prev = mem;
            }
            set_size_and_pinuse_of_inuse_chunk(m, p, remainder_size);
            BOOST_CONTAINER_MEMCHAIN_INCORPORATE_AFTER(pchain, last_it, mem_orig, mem, num_elements);
         }
      }
      if (was_enabled)
         enable_mmap(m);
   }
   return 1;
}

BOOST_CONTAINER_DECL int boost_cont_multialloc_arrays
   (size_t n_elements, const size_t *sizes, size_t element_size, size_t contiguous_elements, boost_cont_memchain *pchain)
{
   int ret = 0;
   mstate ms = (mstate)gm;
   ensure_initialization();
   if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
   }
   else if (!PREACTION(ms)) {
      ret = internal_multialloc_arrays(ms, n_elements, sizes, element_size, contiguous_elements, pchain);
      POSTACTION(ms);
   }
   return ret;
}


/*Doug Lea malloc extensions*/
static boost_cont_malloc_stats_t get_malloc_stats(mstate m)
{
   boost_cont_malloc_stats_t ret;
   ensure_initialization();
   if (!PREACTION(m)) {
      size_t maxfp = 0;
      size_t fp = 0;
      size_t used = 0;
      check_malloc_state(m);
      if (is_initialized(m)) {
         msegmentptr s = &m->seg;
         maxfp = m->max_footprint;
         fp = m->footprint;
         used = fp - (m->topsize + TOP_FOOT_SIZE);

         while (s != 0) {
            mchunkptr q = align_as_chunk(s->base);
            while (segment_holds(s, q) &&
                  q != m->top && q->head != FENCEPOST_HEAD) {
               if (!cinuse(q))
               used -= chunksize(q);
               q = next_chunk(q);
            }
            s = s->next;
         }
      }

      ret.max_system_bytes   = maxfp;
      ret.system_bytes       = fp;
      ret.in_use_bytes       = used;
      POSTACTION(m);
   }
   return ret;
}

BOOST_CONTAINER_DECL size_t boost_cont_size(const void *p)
{  return DL_SIZE_IMPL(p);  }

BOOST_CONTAINER_DECL void* boost_cont_malloc(size_t bytes)
{
   size_t received_bytes;
   ensure_initialization();
   return boost_cont_allocation_command
      (BOOST_CONTAINER_ALLOCATE_NEW, 1, bytes, bytes, &received_bytes, 0).first;
}

BOOST_CONTAINER_DECL void boost_cont_free(void* mem)
{
   mstate ms = (mstate)gm;
   if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
   }
   else if (!PREACTION(ms)) {
      mspace_free_lockless(ms, mem);
      POSTACTION(ms);
   }
}

BOOST_CONTAINER_DECL void* boost_cont_memalign(size_t bytes, size_t alignment)
{
   void *addr;
   ensure_initialization();
   addr = mspace_memalign(gm, alignment, bytes);
   if(addr){
      s_allocated_memory += chunksize(mem2chunk(addr));
   }
   return addr;
}

BOOST_CONTAINER_DECL int boost_cont_multialloc_nodes
   (size_t n_elements, size_t elem_size, size_t contiguous_elements, boost_cont_memchain *pchain)
{
   int ret = 0;
   mstate ms = (mstate)gm;
   ensure_initialization();
   if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
   }
   else if (!PREACTION(ms)) {
      ret = internal_node_multialloc(ms, n_elements, elem_size, contiguous_elements, pchain);
      POSTACTION(ms);
   }
   return ret;
}

BOOST_CONTAINER_DECL size_t boost_cont_footprint()
{
   return ((mstate)gm)->footprint;
}

BOOST_CONTAINER_DECL size_t boost_cont_allocated_memory()
{
   struct mallinfo info = mspace_mallinfo(gm);
   ensure_initialization();
   if(info.ordblks)
      return (size_t)(info.uordblks - (info.ordblks-1)*TOP_FOOT_SIZE);
   else
      return info.uordblks;
}

BOOST_CONTAINER_DECL size_t boost_cont_chunksize(const void *p)
{  return chunksize(mem2chunk(p));   }

BOOST_CONTAINER_DECL int boost_cont_all_deallocated()
{  return !s_allocated_memory;  }

BOOST_CONTAINER_DECL boost_cont_malloc_stats_t boost_cont_malloc_stats()
{
  mstate ms = (mstate)gm;
  if (ok_magic(ms)) {
    return get_malloc_stats(ms);
  }
  else {
    boost_cont_malloc_stats_t r = { 0, 0, 0 };
    USAGE_ERROR_ACTION(ms,ms);
    return r;
  }
}

BOOST_CONTAINER_DECL size_t boost_cont_in_use_memory()
{  return s_allocated_memory;   }

BOOST_CONTAINER_DECL int boost_cont_trim(size_t pad)
{
   ensure_initialization();
   return dlmalloc_trim(pad);
}

BOOST_CONTAINER_DECL int boost_cont_grow
   (void* oldmem, size_t minbytes, size_t maxbytes, size_t *received)
{
   mstate ms = (mstate)gm;
   if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
      return 0;
   }

   if (!PREACTION(ms)) {
      mchunkptr p = mem2chunk(oldmem);
      size_t oldsize = chunksize(p);
      p = try_realloc_chunk_with_min(ms, p, request2size(minbytes), request2size(maxbytes), 0);
      POSTACTION(ms);
      if(p){
         check_inuse_chunk(ms, p);
         *received = DL_SIZE_IMPL(oldmem);
         s_allocated_memory += chunksize(p) - oldsize;
      }
      return 0 != p;
   }
   return 0;
}

BOOST_CONTAINER_DECL int boost_cont_shrink
   (void* oldmem, size_t minbytes, size_t maxbytes, size_t *received, int do_commit)
{
   mstate ms = (mstate)gm;
   if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
      return 0;
   }

   if (!PREACTION(ms)) {
      int ret = internal_shrink(ms, oldmem, minbytes, maxbytes, received, do_commit);
      POSTACTION(ms);
      return 0 != ret;
   }
   return 0;
}


BOOST_CONTAINER_DECL void* boost_cont_alloc
   (size_t minbytes, size_t preferred_bytes, size_t *received_bytes)
{
   //ensure_initialization provided by boost_cont_allocation_command
   return boost_cont_allocation_command
      (BOOST_CONTAINER_ALLOCATE_NEW, 1, minbytes, preferred_bytes, received_bytes, 0).first;
}

BOOST_CONTAINER_DECL void boost_cont_multidealloc(boost_cont_memchain *pchain)
{
   mstate ms = (mstate)gm;
   if (!ok_magic(ms)) {
      (void)ms;
      USAGE_ERROR_ACTION(ms,ms);
   }
   internal_multialloc_free(ms, pchain);
}

BOOST_CONTAINER_DECL int boost_cont_malloc_check()
{
#ifdef DEBUG
   mstate ms = (mstate)gm;
   ensure_initialization();
   if (!ok_magic(ms)) {
      (void)ms;
      USAGE_ERROR_ACTION(ms,ms);
      return 0;
   }
   check_malloc_state(ms);
   return 1;
#else
   return 1;
#endif
}


BOOST_CONTAINER_DECL boost_cont_command_ret_t boost_cont_allocation_command
   (allocation_type command, size_t sizeof_object, size_t limit_size
   , size_t preferred_size, size_t *received_size, void *reuse_ptr)
{
   boost_cont_command_ret_t ret = { 0, 0 };
   ensure_initialization();
   if(command & (BOOST_CONTAINER_SHRINK_IN_PLACE | BOOST_CONTAINER_TRY_SHRINK_IN_PLACE)){
      int success = boost_cont_shrink( reuse_ptr, preferred_size, limit_size
                             , received_size, (command & BOOST_CONTAINER_SHRINK_IN_PLACE));
      ret.first = success ? reuse_ptr : 0;
      return ret;
   }

   *received_size = 0;

   if(limit_size > preferred_size)
      return ret;

   {
      mstate ms = (mstate)gm;

      /*Expand in place*/
      if (!PREACTION(ms)) {
         #if FOOTERS
         if(reuse_ptr){
            mstate m = get_mstate_for(mem2chunk(reuse_ptr));
            if (!ok_magic(m)) {
               USAGE_ERROR_ACTION(m, reuse_ptr);
               return ret;
            }
         }
         #endif
         if(reuse_ptr && (command & (BOOST_CONTAINER_EXPAND_FWD | BOOST_CONTAINER_EXPAND_BWD))){
            void *r = internal_grow_both_sides
               ( ms, command, reuse_ptr, limit_size
               , preferred_size, received_size, sizeof_object, 1);
            if(r){
               ret.first  = r;
               ret.second = 1;
               goto postaction;
            }
         }

         if(command & BOOST_CONTAINER_ALLOCATE_NEW){
            void *addr = mspace_malloc_lockless(ms, preferred_size);
            if(!addr)   addr = mspace_malloc_lockless(ms, limit_size);
            if(addr){
               s_allocated_memory += chunksize(mem2chunk(addr));
            }
            *received_size = DL_SIZE_IMPL(addr);
            ret.first  = addr;
            ret.second = 0;
            if(addr){
               goto postaction;
            }
         }

         //Now try to expand both sides with min size
         if(reuse_ptr && (command & (BOOST_CONTAINER_EXPAND_FWD | BOOST_CONTAINER_EXPAND_BWD))){
            void *r = internal_grow_both_sides
               ( ms, command, reuse_ptr, limit_size
               , preferred_size, received_size, sizeof_object, 0);
            if(r){
               ret.first  = r;
               ret.second = 1;
               goto postaction;
            }
         }
         postaction:
         POSTACTION(ms);
      }
   }
   return ret;
}

BOOST_CONTAINER_DECL int boost_cont_mallopt(int param_number, int value)
{
  return change_mparam(param_number, value);
}

//#ifdef DL_DEBUG_DEFINED
//   #undef DEBUG
//#endif

#ifdef _MSC_VER
#pragma warning (pop)
#endif
