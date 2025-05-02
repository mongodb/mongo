//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_SEGMENT_MANAGER_BASE_HPP
#define BOOST_INTERPROCESS_SEGMENT_MANAGER_BASE_HPP

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
#include <boost/interprocess/exceptions.hpp>
// interprocess/detail
#include <boost/interprocess/detail/type_traits.hpp>
#include <boost/interprocess/detail/utilities.hpp>
// container/detail
#include <boost/container/detail/type_traits.hpp> //alignment_of
#include <boost/container/detail/minimal_char_traits_header.hpp>
#include <boost/container/detail/placement_new.hpp>
// intrusive
#include <boost/intrusive/pointer_traits.hpp>
// move/detail
#include <boost/move/detail/type_traits.hpp> //make_unsigned
#include <boost/move/detail/force_ptr.hpp>
// other boost
#include <boost/assert.hpp>   //BOOST_ASSERT
// std
#include <cstddef>   //std::size_t


namespace boost{
namespace interprocess{

template<class MemoryManager>
class segment_manager_base;

//!An integer that describes the type of the
//!instance constructed in memory
enum instance_type {   anonymous_type, named_type, unique_type, max_allocation_type };

namespace ipcdetail{

template<class MemoryAlgorithm>
class mem_algo_deallocator
{
   void *            m_ptr;
   MemoryAlgorithm & m_algo;

   public:
   mem_algo_deallocator(void *ptr, MemoryAlgorithm &algo)
      :  m_ptr(ptr), m_algo(algo)
   {}

   void release()
   {  m_ptr = 0;  }

   ~mem_algo_deallocator()
   {  if(m_ptr) m_algo.deallocate(m_ptr);  }
};

#if !defined(BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI)
#define BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI 2
#endif   //#if !defined(BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI)

#if (BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI == 1)

template<class size_type>
struct block_header
{
   private:
   const size_type      m_value_bytes;
   const unsigned short m_num_char;
   const unsigned char  m_value_alignment;
   const unsigned char  m_alloc_type_sizeof_char;

   public:
   typedef std::size_t name_len_t;

   block_header(size_type val_bytes
               ,size_type val_alignment
               ,unsigned char al_type
               ,std::size_t szof_char
               ,std::size_t num_char
               )
      :  m_value_bytes(val_bytes)
      ,  m_num_char((unsigned short)num_char)
      ,  m_value_alignment((unsigned char)val_alignment)
      ,  m_alloc_type_sizeof_char( (unsigned char)((al_type << 5u) | ((unsigned char)szof_char & 0x1F)) )
   {};
   
   template<std::size_t>
   size_type total_anonymous_size() const
   {
      return this->value_offset() + m_value_bytes;
   }

   template<std::size_t, class>
   size_type total_named_size(std::size_t namelen) const
   {
      (void)namelen;
      BOOST_ASSERT(namelen == m_num_char);
      return name_offset() + (m_num_char+1u)*sizeof_char();
   }

   template<std::size_t, class, class Header>
   size_type total_named_size_with_header(std::size_t namelen) const
   {
      BOOST_ASSERT(namelen == m_num_char);
      return get_rounded_size
               ( size_type(sizeof(Header))
            , size_type(::boost::container::dtl::alignment_of<block_header<size_type> >::value))
           + this->template total_named_size<0, char>(namelen);
   }

   size_type value_bytes() const
   {  return m_value_bytes;   }

   unsigned char alloc_type() const
   {  return (m_alloc_type_sizeof_char >> 5u)&(unsigned char)0x7;  }

   unsigned char sizeof_char() const
   {  return m_alloc_type_sizeof_char & (unsigned char)0x1F;  }

   template<class CharType>
   CharType *name() const
   {
      return const_cast<CharType*>(move_detail::force_ptr<const CharType*>
         (reinterpret_cast<const char*>(this) + name_offset()));
   }

   unsigned short name_length() const
   {  return m_num_char;   }

   void *value() const
   {
      return const_cast<char*>((reinterpret_cast<const char*>(this) + this->value_offset()));
   }

   template<class T>
   static block_header *block_header_from_value(T *value)
   {
      //      BOOST_ASSERT(is_ptr_aligned(value, algn));
      const std::size_t algn = ::boost::container::dtl::alignment_of<T>::value;
      block_header* hdr =
         const_cast<block_header*>
         (move_detail::force_ptr<const block_header*>(reinterpret_cast<const char*>(value) -
            get_rounded_size(sizeof(block_header), algn)));

      //Some sanity checks
      BOOST_ASSERT(hdr->m_value_alignment == algn);
      BOOST_ASSERT(hdr->m_value_bytes % sizeof(T) == 0);
      return hdr;
   }

   template<class Header>
   static block_header *from_first_header(Header *header)
   {
      block_header * hdr =
         move_detail::force_ptr<block_header*>(reinterpret_cast<char*>(header) +
       get_rounded_size( size_type(sizeof(Header))
                       , size_type(::boost::container::dtl::alignment_of<block_header >::value)));
      //Some sanity checks
      return hdr;
   }

   template<class Header>
   static const block_header *from_first_header(const Header *header)
   {  return from_first_header(const_cast<Header*>(header));   }

   template<class Header>
   static Header *to_first_header(block_header *bheader)
   {
      Header * hdr =
         move_detail::force_ptr<Header*>(reinterpret_cast<char*>(bheader) -
       get_rounded_size( size_type(sizeof(Header))
                       , size_type(::boost::container::dtl::alignment_of<block_header >::value)));
      //Some sanity checks
      return hdr;
   }

   template<std::size_t>
   static size_type front_space_without_header()
   {  return 0u;  }

   template<std::size_t, class>
   static size_type front_space_with_header()
   {  return 0u;  }

   void store_name_length(std::size_t)
   {}

   private:
   size_type value_offset() const
   {
      return get_rounded_size(size_type(sizeof(block_header)), size_type(m_value_alignment));
   }

   size_type name_offset() const
   {
      return this->value_offset() + get_rounded_size(size_type(m_value_bytes), size_type(sizeof_char()));
   }
};

#elif (BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI == 2)

template <class BlockHeader, class Header>
struct sm_between_headers
{
   BOOST_STATIC_CONSTEXPR std::size_t value
      = sizeof(Header)
      + ct_rounded_size< sizeof(BlockHeader), boost::move_detail::alignment_of<Header>::value>::value
      - sizeof(BlockHeader);
};

template <std::size_t TypeAlignment, class BlockHeader, class Header>
struct sg_offsets_with_header
{
   private:
   BOOST_STATIC_CONSTEXPR std::size_t between_headers = sm_between_headers<BlockHeader, Header>::value;
   BOOST_STATIC_CONSTEXPR std::size_t both_headers = between_headers + sizeof(BlockHeader);
   BOOST_STATIC_CONSTEXPR std::size_t total_prefix = ct_rounded_size<both_headers, TypeAlignment>::value;

   public:
   BOOST_STATIC_CONSTEXPR std::size_t block_header_prefix = total_prefix - sizeof(BlockHeader);
   BOOST_STATIC_CONSTEXPR std::size_t front_space = total_prefix - both_headers;

   BOOST_INTERPROCESS_STATIC_ASSERT((total_prefix % TypeAlignment) == 0);
   BOOST_INTERPROCESS_STATIC_ASSERT((front_space % boost::move_detail::alignment_of<Header>::value) == 0);
   BOOST_INTERPROCESS_STATIC_ASSERT((block_header_prefix % boost::move_detail::alignment_of<BlockHeader>::value) == 0);
   BOOST_INTERPROCESS_STATIC_ASSERT(total_prefix == (sizeof(BlockHeader) + sizeof(Header) + front_space + (between_headers - sizeof(Header))));
};

template <std::size_t TypeAlignment, class BlockHeader>
struct sg_offsets_without_header
{
   BOOST_STATIC_CONSTEXPR std::size_t total_prefix = ct_rounded_size<sizeof(BlockHeader), TypeAlignment>::value;

   public:
   BOOST_STATIC_CONSTEXPR std::size_t block_header_prefix = total_prefix - sizeof(BlockHeader);
   BOOST_STATIC_CONSTEXPR std::size_t front_space = block_header_prefix;
};

template<class size_type>
struct block_header
{
   private:
   const size_type  m_alloc_type  : 2;
   const size_type  m_value_bytes : sizeof(size_type)*CHAR_BIT - 2u;

   public:
   typedef unsigned short name_len_t;

   block_header(size_type val_bytes
               ,size_type 
               ,unsigned char al_type
               ,std::size_t
               ,std::size_t
               )
      : m_alloc_type(al_type & 3u)
      , m_value_bytes(val_bytes & (~size_type(0) >> 2u))
   {};

   template<std::size_t TypeAlignment>
   size_type total_anonymous_size() const
   {
      BOOST_CONSTEXPR_OR_CONST std::size_t block_header_prefix =
         sg_offsets_without_header<TypeAlignment, block_header>::block_header_prefix;
      return block_header_prefix + this->value_offset() + m_value_bytes;
   }

   template<std::size_t TypeAlignment, class CharType>
   size_type total_named_size(std::size_t namelen) const
   {
      BOOST_CONSTEXPR_OR_CONST std::size_t block_header_prefix =
         sg_offsets_without_header<TypeAlignment, block_header>::block_header_prefix;
      return block_header_prefix
         + name_offset< ::boost::move_detail::alignment_of<CharType>::value>()
         + (namelen + 1u) * sizeof(CharType);
   }

   template<std::size_t TypeAlignment, class CharType, class Header>
   size_type total_named_size_with_header(std::size_t namelen) const
   {
      typedef sg_offsets_with_header<TypeAlignment, block_header, Header> offsets_t;
      return offsets_t::block_header_prefix
         + name_offset< ::boost::move_detail::alignment_of<CharType>::value>()
         + (namelen + 1u) * sizeof(CharType);
   }

   size_type value_bytes() const
   {  return m_value_bytes;   }

   unsigned char alloc_type() const
   {  return m_alloc_type;  }

   template<class CharType>
   CharType *name() const
   {
      return const_cast<CharType*>(move_detail::force_ptr<const CharType*>
         (reinterpret_cast<const char*>(this) +
          this->template name_offset< ::boost::move_detail::alignment_of<CharType>::value>()));
   }

   name_len_t name_length() const
   {
      if(m_alloc_type == anonymous_type)
         return 0;
      return *(move_detail::force_ptr<const name_len_t*>
         (reinterpret_cast<const char*>(this) + this->name_length_offset()));
   }

   void *value() const
   {  return const_cast<char*>((reinterpret_cast<const char*>(this) + this->value_offset()));  }

   template<class T>
   static block_header *block_header_from_value(T *value)
   {
      BOOST_ASSERT(is_ptr_aligned(value, ::boost::container::dtl::alignment_of<T>::value));
      block_header* hdr =
         const_cast<block_header*>
            (move_detail::force_ptr<const block_header*>
               (reinterpret_cast<const char*>(value) - value_offset()));

      //Some sanity checks
      BOOST_ASSERT(hdr->m_value_bytes % sizeof(T) == 0);
      return hdr;
   }

   template<class Header>
   static block_header *from_first_header(Header *header)
   {
      BOOST_ASSERT(is_ptr_aligned(header));
      block_header * const hdr = move_detail::force_ptr<block_header*>(
            reinterpret_cast<char*>(header) + sm_between_headers<block_header, Header>::value);
      //Some sanity checks
      BOOST_ASSERT(is_ptr_aligned(hdr));
      return hdr;
   }

   template<class Header>
   static const block_header *from_first_header(const Header *header)
   {  return from_first_header(const_cast<Header*>(header));   }

   template<class Header>
   static Header *to_first_header(block_header *bheader)
   {
      BOOST_ASSERT(is_ptr_aligned(bheader));
      Header * hdr = move_detail::force_ptr<Header*>(
         reinterpret_cast<char*>(bheader) - sm_between_headers<block_header, Header>::value);
      //Some sanity checks
      BOOST_ASSERT(is_ptr_aligned(hdr));
      return hdr;
   }

   template<std::size_t TypeAlignment, class Header>
   static size_type front_space_with_header()
   {  return sg_offsets_with_header<TypeAlignment, block_header, Header>::front_space; }

   template<std::size_t TypeAlignment>
   static size_type front_space_without_header()
   {  return sg_offsets_without_header<TypeAlignment, block_header>::front_space; }

   void store_name_length(name_len_t namelen)
   {
      ::new( reinterpret_cast<char*>(this) + this->name_length_offset()
           , boost_container_new_t()
           ) name_len_t(namelen);
   }

   private:

   static size_type value_offset()
   {  return size_type(sizeof(block_header));   }

   template<std::size_t CharAlign>
   size_type name_offset() const
   {  return get_rounded_size(this->name_length_offset()+sizeof(name_len_t), CharAlign);  }

   size_type name_length_offset() const
   {
      return this->value_offset() + get_rounded_size(m_value_bytes, ::boost::move_detail::alignment_of<name_len_t>::value);
   }
};


#else //(BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI == )

#error "Incorrect BOOST_INTERPROCESS_SEGMENT_MANAGER_ABI value!"

#endif

template<class CharT>
struct intrusive_compare_key
{
   typedef CharT char_type;

   intrusive_compare_key(const CharT* str_, std::size_t len_)
      : mp_str(str_), m_len(len_)
   {}

   const CharT* str() const
   {
      return mp_str;
   }

   std::size_t len() const
   {
      return m_len;
   }

   const CharT* mp_str;
   std::size_t    m_len;
};

//!This struct indicates an anonymous object creation
//!allocation
template<instance_type type>
class instance_t
{
   instance_t(){}
};

template<class T>
struct char_if_void
{
   typedef T type;
};

template<>
struct char_if_void<void>
{
   typedef char type;
};

typedef instance_t<anonymous_type>  anonymous_instance_t;
typedef instance_t<unique_type>     unique_instance_t;


template<class Hook, class CharType, class SizeType>
struct intrusive_value_type_impl
   :  public Hook
{
   private:
   //Non-copyable
   intrusive_value_type_impl(const intrusive_value_type_impl &);
   intrusive_value_type_impl& operator=(const intrusive_value_type_impl &);

   public:
   typedef CharType char_type;
   typedef SizeType size_type;
   typedef block_header<size_type> block_header_t;

   intrusive_value_type_impl(){}

   CharType *name() const
   {  return get_block_header()->template name<CharType>(); }

   unsigned short name_length() const
   {  return get_block_header()->name_length(); }

   void *value() const
   {  return get_block_header()->value(); }

   private:
   const block_header_t *get_block_header() const
   {  return block_header_t::from_first_header(this); }
};

template<class CharType>
class char_ptr_holder
{
   public:
   char_ptr_holder(const CharType *name)
      : m_name(name)
   {}

   char_ptr_holder(const anonymous_instance_t *)
      : m_name(static_cast<CharType*>(0))
   {}

   char_ptr_holder(const unique_instance_t *)
      : m_name(reinterpret_cast<CharType*>(-1))
   {}

   operator const CharType *()
   {  return m_name;  }

   const CharType *get() const
   {  return m_name;  }

   bool is_unique() const
   {  return m_name == reinterpret_cast<CharType*>(-1);  }

   bool is_anonymous() const
   {  return m_name == static_cast<CharType*>(0);  }

   private:
   const CharType *m_name;
};

//!The key of the the named allocation information index. Stores an offset pointer
//!to a null terminated string and the length of the string to speed up sorting
template<class CharT, class VoidPointer>
struct index_key
{
   typedef typename boost::intrusive::
      pointer_traits<VoidPointer>::template
         rebind_pointer<const CharT>::type               const_char_ptr_t;
   typedef CharT                                         char_type;
   typedef typename boost::intrusive::pointer_traits<const_char_ptr_t>::difference_type difference_type;
   typedef typename boost::move_detail::make_unsigned<difference_type>::type size_type;

   private:
   //Offset pointer to the object's name
   const_char_ptr_t  mp_str;
   //Length of the name buffer (null NOT included)
   size_type         m_len;
   public:

   //!Constructor of the key
   index_key (const char_type *nm, size_type length)
      : mp_str(nm), m_len(length)
   {}

   //!Less than function for index ordering
   bool operator < (const index_key & right) const
   {
      return (m_len < right.m_len) ||
               (m_len == right.m_len &&
                std::char_traits<char_type>::compare
                  (to_raw_pointer(mp_str),to_raw_pointer(right.mp_str), m_len) < 0);
   }

   //!Equal to function for index ordering
   bool operator == (const index_key & right) const
   {
      return   m_len == right.m_len &&
               std::char_traits<char_type>::compare
                  (to_raw_pointer(mp_str), to_raw_pointer(right.mp_str), m_len) == 0;
   }

   void name(const CharT *nm)
   {  mp_str = nm; }

   void name_length(size_type len)
   {  m_len = len; }

   const CharT *name() const
   {  return to_raw_pointer(mp_str); }

   size_type name_length() const
   {  return m_len; }
};

//!The index_data stores a pointer to a buffer and the element count needed
//!to know how many destructors must be called when calling destroy
template<class VoidPointer>
struct index_data
{
   typedef VoidPointer void_pointer;
   void_pointer    m_ptr;
   explicit index_data(void *ptr) : m_ptr(ptr){}

   void *value() const
   {  return static_cast<void*>(to_raw_pointer(m_ptr));  }
};

template<class MemoryAlgorithm>
struct segment_manager_base_type
{  typedef segment_manager_base<MemoryAlgorithm> type;   };

template<class CharT, class MemoryAlgorithm>
struct index_config
{
   typedef typename MemoryAlgorithm::void_pointer        void_pointer;
   typedef CharT                                         char_type;
   typedef index_key<CharT, void_pointer>                key_type;
   typedef index_data<void_pointer>                      mapped_type;
   typedef typename segment_manager_base_type
      <MemoryAlgorithm>::type                            segment_manager_base;

   template<class HeaderBase>
   struct intrusive_value_type
   {
      typedef intrusive_value_type_impl
         < HeaderBase
         , CharT
         , typename segment_manager_base::size_type
         >  type;
   };

   typedef intrusive_compare_key<CharT>                  compare_key_type;
};

template<class Iterator, bool intrusive>
class segment_manager_iterator_value_adaptor
{
   typedef typename Iterator::value_type        iterator_val_t;
   typedef typename iterator_val_t::char_type   char_type;

   public:
   segment_manager_iterator_value_adaptor(const typename Iterator::value_type &val)
      :  m_val(&val)
   {}

   const char_type *name() const
   {  return m_val->name(); }

   unsigned short name_length() const
   {  return m_val->name_length(); }

   const void *value() const
   {  return m_val->value(); }

   const typename Iterator::value_type *m_val;
};


template<class Iterator>
class segment_manager_iterator_value_adaptor<Iterator, false>
{
   typedef typename Iterator::value_type        iterator_val_t;
   typedef typename iterator_val_t::first_type  first_type;
   typedef typename iterator_val_t::second_type second_type;
   typedef typename first_type::char_type       char_type;
   typedef typename first_type::size_type       size_type;

   public:
   segment_manager_iterator_value_adaptor(const typename Iterator::value_type &val)
      :  m_val(&val)
   {}

   const char_type *name() const
   {  return m_val->first.name(); }

   size_type name_length() const
   {  return m_val->first.name_length(); }

   const void *value() const
   {
      return move_detail::force_ptr<block_header<size_type>*>
         (to_raw_pointer(m_val->second.m_ptr))->value();
   }

   const typename Iterator::value_type *m_val;
};

template<class Iterator, bool intrusive>
struct segment_manager_iterator_transform
{
   typedef segment_manager_iterator_value_adaptor<Iterator, intrusive> result_type;

   template <class T> result_type operator()(const T &arg) const
   {  return result_type(arg); }
};


template<class T>
inline T* null_or_bad_alloc(bool dothrow)
{
   if (dothrow)
      throw bad_alloc();
   return 0;
}

template<class T>
inline T* null_or_already_exists(bool dothrow)
{
   if (dothrow)
      throw interprocess_exception(already_exists_error);
   return 0;
}

}  //namespace ipcdetail {

//These pointers are the ones the user will use to
//indicate previous allocation types
static const ipcdetail::anonymous_instance_t   * anonymous_instance = 0;
static const ipcdetail::unique_instance_t      * unique_instance = 0;

namespace ipcdetail_really_deep_namespace {

//Otherwise, gcc issues a warning of previously defined
//anonymous_instance and unique_instance
struct dummy
{
   dummy()
   {
      (void)anonymous_instance;
      (void)unique_instance;
   }
};

}  //detail_really_deep_namespace

}} //namespace boost { namespace interprocess

#include <boost/interprocess/detail/config_end.hpp>

#endif //#ifndef BOOST_INTERPROCESS_SEGMENT_MANAGER_BASE_HPP

