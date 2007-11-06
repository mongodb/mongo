/* Copyright 2003-2007 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_RANDOM_ACCESS_INDEX_HPP
#define BOOST_MULTI_INDEX_RANDOM_ACCESS_INDEX_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <algorithm>
#include <boost/call_traits.hpp>
#include <boost/detail/no_exceptions_support.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/iterator/reverse_iterator.hpp>
#include <boost/mpl/push_front.hpp>
#include <boost/multi_index/detail/access_specifier.hpp>
#include <boost/multi_index/detail/index_node_base.hpp>
#include <boost/multi_index/detail/rnd_node_iterator.hpp>
#include <boost/multi_index/detail/rnd_index_node.hpp>
#include <boost/multi_index/detail/rnd_index_ops.hpp>
#include <boost/multi_index/detail/rnd_index_ptr_array.hpp>
#include <boost/multi_index/detail/safe_ctr_proxy.hpp>
#include <boost/multi_index/detail/safe_mode.hpp>
#include <boost/multi_index/detail/scope_guard.hpp>
#include <boost/multi_index/random_access_index_fwd.hpp>
#include <boost/throw_exception.hpp> 
#include <boost/tuple/tuple.hpp>
#include <cstddef>
#include <functional>
#include <stdexcept> 
#include <utility>

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
#include <boost/bind.hpp>
#include <boost/multi_index/detail/rnd_index_loader.hpp>
#endif

#if defined(BOOST_MULTI_INDEX_ENABLE_INVARIANT_CHECKING)
#define BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT                          \
  detail::scope_guard BOOST_JOIN(check_invariant_,__LINE__)=                 \
    detail::make_obj_guard(*this,&random_access_index::check_invariant_);    \
  BOOST_JOIN(check_invariant_,__LINE__).touch();
#else
#define BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* random_access_index adds a layer of random access indexing
 * to a given Super
 */

template<typename SuperMeta,typename TagList>
class random_access_index:
  BOOST_MULTI_INDEX_PROTECTED_IF_MEMBER_TEMPLATE_FRIENDS SuperMeta::type

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
#if BOOST_WORKAROUND(BOOST_MSVC,<1300)
  ,public safe_ctr_proxy_impl<
    rnd_node_iterator<
      random_access_index_node<typename SuperMeta::type::node_type> >,
    random_access_index<SuperMeta,TagList> >
#else
  ,public safe_mode::safe_container<
    random_access_index<SuperMeta,TagList> >
#endif
#endif

{ 
#if defined(BOOST_MULTI_INDEX_ENABLE_INVARIANT_CHECKING)&&\
    BOOST_WORKAROUND(__MWERKS__,<=0x3003)
/* The "ISO C++ Template Parser" option in CW8.3 has a problem with the
 * lifetime of const references bound to temporaries --precisely what
 * scopeguards are.
 */

#pragma parse_mfunc_templ off
#endif

  typedef typename SuperMeta::type                 super;

protected:
  typedef random_access_index_node<
    typename super::node_type>                     node_type;

public:
  /* types */

  typedef typename node_type::value_type           value_type;
  typedef tuples::null_type                        ctor_args;
  typedef typename super::final_allocator_type     allocator_type;
  typedef typename allocator_type::reference       reference;
  typedef typename allocator_type::const_reference const_reference;

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
#if BOOST_WORKAROUND(BOOST_MSVC,<1300)
  typedef safe_mode::safe_iterator<
    rnd_node_iterator<node_type>,
    safe_ctr_proxy<
      rnd_node_iterator<node_type> > >             iterator;
#else
  typedef safe_mode::safe_iterator<
    rnd_node_iterator<node_type>,
    random_access_index>                           iterator;
#endif
#else
  typedef rnd_node_iterator<node_type>             iterator;
#endif

  typedef iterator                                 const_iterator;

  typedef std::size_t                              size_type;      
  typedef std::ptrdiff_t                           difference_type;
  typedef typename allocator_type::pointer         pointer;
  typedef typename allocator_type::const_pointer   const_pointer;
  typedef typename
    boost::reverse_iterator<iterator>              reverse_iterator;
  typedef typename
    boost::reverse_iterator<const_iterator>        const_reverse_iterator;
  typedef TagList                                  tag_list;

protected:
  typedef typename super::final_node_type     final_node_type;
  typedef tuples::cons<
    ctor_args, 
    typename super::ctor_args_list>           ctor_args_list;
  typedef typename mpl::push_front<
    typename super::index_type_list,
    random_access_index>::type                index_type_list;
  typedef typename mpl::push_front<
    typename super::iterator_type_list,
    iterator>::type                           iterator_type_list;
  typedef typename mpl::push_front<
    typename super::const_iterator_type_list,
    const_iterator>::type                     const_iterator_type_list;
  typedef typename super::copy_map_type       copy_map_type;

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
  typedef typename super::index_saver_type    index_saver_type;
  typedef typename super::index_loader_type   index_loader_type;
#endif

private:
#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
#if BOOST_WORKAROUND(BOOST_MSVC,<1300)
  typedef safe_ctr_proxy_impl<
    rnd_node_iterator<node_type>,
    random_access_index>                      safe_super;
#else
  typedef safe_mode::safe_container<
    random_access_index>                      safe_super;
#endif
#endif

  typedef typename call_traits<
    value_type>::param_type                   value_param_type;

public:

  /* construct/copy/destroy
   * Default and copy ctors are in the protected section as indices are
   * not supposed to be created on their own. No range ctor either.
   */

  random_access_index<SuperMeta,TagList>& operator=(
    const random_access_index<SuperMeta,TagList>& x)
  {
    this->final()=x.final();
    return *this;
  }

  template <class InputIterator>
  void assign(InputIterator first,InputIterator last)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    clear();
    for(;first!=last;++first)push_back(*first);
  }

  void assign(size_type n,value_param_type value)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    clear();
    for(size_type i=0;i<n;++i)push_back(value);
  }
    
  allocator_type get_allocator()const
  {
    return this->final().get_allocator();
  }

  /* iterators */

  iterator               begin()
    {return make_iterator(node_type::from_impl(*ptrs.begin()));}
  const_iterator         begin()const
    {return make_iterator(node_type::from_impl(*ptrs.begin()));}
  iterator               end(){return make_iterator(header());}
  const_iterator         end()const{return make_iterator(header());}
  reverse_iterator       rbegin(){return make_reverse_iterator(end());}
  const_reverse_iterator rbegin()const{return make_reverse_iterator(end());}
  reverse_iterator       rend(){return make_reverse_iterator(begin());}
  const_reverse_iterator rend()const{return make_reverse_iterator(begin());}

  /* capacity */

  bool      empty()const{return this->final_empty_();}
  size_type size()const{return this->final_size_();}
  size_type max_size()const{return this->final_max_size_();}
  size_type capacity()const{return ptrs.capacity();}
  void      reserve(size_type n){ptrs.reserve(n);}

  void resize(size_type n,value_param_type x=value_type())
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    if(n>size())insert(end(),n-size(),x);
    else if(n<size())erase(begin()+n,end());
  }

  /* access: no non-const versions provided as random_access_index
   * handles const elements.
   */

  const_reference operator[](size_type n)const
  {
    BOOST_MULTI_INDEX_SAFE_MODE_ASSERT(n<size(),safe_mode::out_of_bounds);
    return node_type::from_impl(*ptrs.at(n))->value();
  }

  const_reference at(size_type n)const
  {
    if(n>=size())throw_exception(std::out_of_range("random access index"));
    return node_type::from_impl(*ptrs.at(n))->value();
  }

  const_reference front()const{return operator[](0);}
  const_reference back()const{return operator[](size()-1);}

  /* modifiers */

  std::pair<iterator,bool> push_front(value_param_type x)
                             {return insert(begin(),x);}
  void                     pop_front(){erase(begin());}
  std::pair<iterator,bool> push_back(value_param_type x)
                             {return insert(end(),x);}
  void                     pop_back(){erase(--end());}

  std::pair<iterator,bool> insert(iterator position,value_param_type x)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    std::pair<final_node_type*,bool> p=this->final_insert_(x);
    if(p.second&&position.get_node()!=header()){
      relocate(position.get_node(),p.first);
    }
    return std::pair<iterator,bool>(make_iterator(p.first),p.second);
  }

  void insert(iterator position,size_type n,value_param_type x)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    size_type s=0;
    BOOST_TRY{
      while(n--){
        if(push_back(x).second)++s;
      }
    }
    BOOST_CATCH(...){
      relocate(position,end()-s,end());
      BOOST_RETHROW;
    }
    BOOST_CATCH_END
    relocate(position,end()-s,end());
  }
 
  template<typename InputIterator>
  void insert(iterator position,InputIterator first,InputIterator last)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    size_type s=0;
    BOOST_TRY{
      for(;first!=last;++first){
        if(push_back(*first).second)++s;
      }
    }
    BOOST_CATCH(...){
      relocate(position,end()-s,end());
      BOOST_RETHROW;
    }
    BOOST_CATCH_END
    relocate(position,end()-s,end());
  }

  iterator erase(iterator position)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_DEREFERENCEABLE_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    this->final_erase_(static_cast<final_node_type*>(position++.get_node()));
    return position;
  }
  
  iterator erase(iterator first,iterator last)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(first);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(last);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(first,*this);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(last,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_RANGE(first,last);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    difference_type n=last-first;
    relocate(end(),first,last);
    while(n--)pop_back();
    return last;
  }

  bool replace(iterator position,value_param_type x)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_DEREFERENCEABLE_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    return this->final_replace_(
      x,static_cast<final_node_type*>(position.get_node()));
  }

  template<typename Modifier>
  bool modify(iterator position,Modifier mod)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_DEREFERENCEABLE_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    /* MSVC++ 6.0 optimizer on safe mode code chokes if this
     * this is not added. Left it for all compilers as it does no
     * harm.
     */

    position.detach();
#endif

    return this->final_modify_(
      mod,static_cast<final_node_type*>(position.get_node()));
  }

  void swap(random_access_index<SuperMeta,TagList>& x)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    this->final_swap_(x.final());
  }

  void clear()
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    this->final_clear_();
  }

  /* list operations */

  void splice(iterator position,random_access_index<SuperMeta,TagList>& x)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_CHECK_DIFFERENT_CONTAINER(*this,x);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    iterator  first=x.begin(),last=x.end();
    size_type n=0;
    BOOST_TRY{
      while(first!=last){
        if(push_back(*first).second){
          first=x.erase(first);
          ++n;
        }
        else ++first;
      }
    }
    BOOST_CATCH(...){
      relocate(position,end()-n,end());
      BOOST_RETHROW;
    }
    BOOST_CATCH_END
    relocate(position,end()-n,end());
  }

  void splice(
    iterator position,random_access_index<SuperMeta,TagList>& x,iterator i)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(i);
    BOOST_MULTI_INDEX_CHECK_DEREFERENCEABLE_ITERATOR(i);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(i,x);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    if(&x==this)relocate(position,i);
    else{
      if(insert(position,*i).second){

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    /* MSVC++ 6.0 optimizer has a hard time with safe mode, and the following
     * workaround is needed. Left it for all compilers as it does no
     * harm.
     */
        i.detach();
        x.erase(x.make_iterator(i.get_node()));
#else
        x.erase(i);
#endif

      }
    }
  }

  void splice(
    iterator position,random_access_index<SuperMeta,TagList>& x,
    iterator first,iterator last)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(first);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(last);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(first,x);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(last,x);
    BOOST_MULTI_INDEX_CHECK_VALID_RANGE(first,last);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    if(&x==this)relocate(position,first,last);
    else{
      size_type n=0;
      BOOST_TRY{
        while(first!=last){
          if(push_back(*first).second){
            first=x.erase(first);
            ++n;
          }
          else ++first;
        }
      }
      BOOST_CATCH(...){
        relocate(position,end()-n,end());
        BOOST_RETHROW;
      }
      BOOST_CATCH_END
      relocate(position,end()-n,end());
    }
  }

  void remove(value_param_type value)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    difference_type n=
      end()-make_iterator(
        random_access_index_remove<node_type>(
          ptrs,std::bind2nd(std::equal_to<value_type>(),value)));
    while(n--)pop_back();
  }

  template<typename Predicate>
  void remove_if(Predicate pred)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    difference_type n=
      end()-make_iterator(random_access_index_remove<node_type>(ptrs,pred));
    while(n--)pop_back();
  }

  void unique()
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    difference_type n=
      end()-make_iterator(
        random_access_index_unique<node_type>(
          ptrs,std::equal_to<value_type>()));
    while(n--)pop_back();
  }

  template <class BinaryPredicate>
  void unique(BinaryPredicate binary_pred)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    difference_type n=
      end()-make_iterator(
        random_access_index_unique<node_type>(ptrs,binary_pred));
    while(n--)pop_back();
  }

  void merge(random_access_index<SuperMeta,TagList>& x)
  {
    if(this!=&x){
      BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
      size_type s=size();
      splice(end(),x);
      random_access_index_inplace_merge<node_type>(
        get_allocator(),ptrs,ptrs.at(s),std::less<value_type>());
    }
  }

  template <typename Compare>
  void merge(random_access_index<SuperMeta,TagList>& x,Compare comp)
  {
    if(this!=&x){
      BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
      size_type s=size();
      splice(end(),x);
      random_access_index_inplace_merge<node_type>(
        get_allocator(),ptrs,ptrs.at(s),comp);
    }
  }

  void sort()
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    random_access_index_sort<node_type>(
      get_allocator(),ptrs,std::less<value_type>());
  }

  template <typename Compare>
  void sort(Compare comp)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    random_access_index_sort<node_type>(
      get_allocator(),ptrs,comp);
  }

  void reverse()
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    random_access_index_node_impl::reverse(ptrs.begin(),ptrs.end());
  }

  /* rearrange operations */

  void relocate(iterator position,iterator i)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(i);
    BOOST_MULTI_INDEX_CHECK_DEREFERENCEABLE_ITERATOR(i);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(i,*this);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    if(position!=i)relocate(position.get_node(),i.get_node());
  }

  void relocate(iterator position,iterator first,iterator last)
  {
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(position);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(position,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(first);
    BOOST_MULTI_INDEX_CHECK_VALID_ITERATOR(last);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(first,*this);
    BOOST_MULTI_INDEX_CHECK_IS_OWNER(last,*this);
    BOOST_MULTI_INDEX_CHECK_VALID_RANGE(first,last);
    BOOST_MULTI_INDEX_CHECK_OUTSIDE_RANGE(position,first,last);
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    if(position!=last)relocate(
      position.get_node(),first.get_node(),last.get_node());
  }

  template<typename InputIterator>
  void rearrange(InputIterator first)
  {
    BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT;
    for(random_access_index_node_impl** p0=ptrs.begin(),** p0_end=ptrs.end();
        p0!=p0_end;++first,++p0){
      const value_type&               v1=*first;
      random_access_index_node_impl** p1=node_from_value<node_type>(&v1)->up();

      std::swap(*p0,*p1);
      (*p0)->up()=p0;
      (*p1)->up()=p1;
    }
  }
    
BOOST_MULTI_INDEX_PROTECTED_IF_MEMBER_TEMPLATE_FRIENDS:
  random_access_index(
    const ctor_args_list& args_list,const allocator_type& al):
    super(args_list.get_tail(),al),
    ptrs(al,header()->impl(),0)
  {
  }

  random_access_index(const random_access_index<SuperMeta,TagList>& x):
    super(x),

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    safe_super(),
#endif

    ptrs(x.get_allocator(),header()->impl(),x.size())
  {
    /* The actual copying takes place in subsequent call to copy_().
     */
  }

  ~random_access_index()
  {
    /* the container is guaranteed to be empty by now */
  }

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
  iterator       make_iterator(node_type* node){return iterator(node,this);}
  const_iterator make_iterator(node_type* node)const
    {return const_iterator(node,const_cast<random_access_index*>(this));}
#else
  iterator       make_iterator(node_type* node){return iterator(node);}
  const_iterator make_iterator(node_type* node)const
                   {return const_iterator(node);}
#endif

  void copy_(
    const random_access_index<SuperMeta,TagList>& x,const copy_map_type& map)
  {
    for(random_access_index_node_impl** begin_org=x.ptrs.begin(),
                                     ** begin_cpy=ptrs.begin(),
                                     ** end_org=x.ptrs.end();
        begin_org!=end_org;++begin_org,++begin_cpy){
      *begin_cpy=
         static_cast<node_type*>(
           map.find(
             static_cast<final_node_type*>(
               node_type::from_impl(*begin_org))))->impl();
      (*begin_cpy)->up()=begin_cpy;
    }

    super::copy_(x,map);
  }

  node_type* insert_(value_param_type v,node_type* x)
  {
    ptrs.room_for_one();
    node_type* res=static_cast<node_type*>(super::insert_(v,x));
    if(res==x)ptrs.push_back(x->impl());
    return res;
  }

  node_type* insert_(value_param_type v,node_type* position,node_type* x)
  {
    ptrs.room_for_one();
    node_type* res=static_cast<node_type*>(super::insert_(v,position,x));
    if(res==x)ptrs.push_back(x->impl());
    return res;
  }

  void erase_(node_type* x)
  {
    ptrs.erase(x->impl());
    super::erase_(x);

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    detach_iterators(x);
#endif
  }

  void delete_all_nodes_()
  {
    for(random_access_index_node_impl** x=ptrs.begin(),**x_end=ptrs.end();
        x!=x_end;++x){
      this->final_delete_node_(
        static_cast<final_node_type*>(node_type::from_impl(*x)));
    }
  }

  void clear_()
  {
    super::clear_();
    ptrs.clear();

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    safe_super::detach_dereferenceable_iterators();
#endif
  }

  void swap_(random_access_index<SuperMeta,TagList>& x)
  {
    ptrs.swap(x.ptrs);

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
    safe_super::swap(x);
#endif

    super::swap_(x);
  }

  bool replace_(value_param_type v,node_type* x)
  {
    return super::replace_(v,x);
  }

  bool modify_(node_type* x)
  {
    BOOST_TRY{
      if(!super::modify_(x)){
        ptrs.erase(x->impl());

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
        detach_iterators(x);
#endif

        return false;
      }
      else return true;
    }
    BOOST_CATCH(...){
      ptrs.erase(x->impl());

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
      detach_iterators(x);
#endif

      BOOST_RETHROW;
    }
    BOOST_CATCH_END
  }

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
  /* serialization */

  template<typename Archive>
  void save_(
    Archive& ar,const unsigned int version,const index_saver_type& sm)const
  {
    sm.save(begin(),end(),ar,version);
    super::save_(ar,version,sm);
  }

  template<typename Archive>
  void load_(
    Archive& ar,const unsigned int version,const index_loader_type& lm)
  {
    {
      typedef random_access_index_loader<node_type,allocator_type> loader;

      loader ld(get_allocator(),ptrs);
      lm.load(::boost::bind(&loader::rearrange,&ld,_1,_2),ar,version);
    } /* exit scope so that ld frees its resources */
    super::load_(ar,version,lm);
  }
#endif

#if defined(BOOST_MULTI_INDEX_ENABLE_INVARIANT_CHECKING)
  /* invariant stuff */

  bool invariant_()const
  {
    if(size()>capacity())return false;
    if(size()==0||begin()==end()){
      if(size()!=0||begin()!=end())return false;
    }
    else{
      size_type s=0;
      for(const_iterator it=begin(),it_end=end();;++it,++s){
        if(*(it.get_node()->up())!=it.get_node()->impl())return false;
        if(it==it_end)break;
      }
      if(s!=size())return false;
    }

    return super::invariant_();
  }

  /* This forwarding function eases things for the boost::mem_fn construct
   * in BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT. Actually,
   * final_check_invariant is already an inherited member function of index.
   */
  void check_invariant_()const{this->final_check_invariant_();}
#endif

private:
  node_type* header()const{return this->final_header();}

  static void relocate(node_type* position,node_type* x)
  {
    random_access_index_node_impl::relocate(position->up(),x->up());
  }

  static void relocate(node_type* position,node_type* first,node_type* last)
  {
    random_access_index_node_impl::relocate(
      position->up(),first->up(),last->up());
  }

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
  void detach_iterators(node_type* x)
  {
    iterator it=make_iterator(x);
    safe_mode::detach_equivalent_iterators(it);
  }
#endif

  random_access_index_ptr_array<typename super::final_allocator_type> ptrs;

#if defined(BOOST_MULTI_INDEX_ENABLE_INVARIANT_CHECKING)&&\
    BOOST_WORKAROUND(__MWERKS__,<=0x3003)
#pragma parse_mfunc_templ reset
#endif
};

/* comparison */

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator==(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return x.size()==y.size()&&std::equal(x.begin(),x.end(),y.begin());
}

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator<(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return std::lexicographical_compare(x.begin(),x.end(),y.begin(),y.end());
}

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator!=(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return !(x==y);
}

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator>(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return y<x;
}

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator>=(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return !(x<y);
}

template<
  typename SuperMeta1,typename TagList1,
  typename SuperMeta2,typename TagList2
>
bool operator<=(
  const random_access_index<SuperMeta1,TagList1>& x,
  const random_access_index<SuperMeta2,TagList2>& y)
{
  return !(x>y);
}

/*  specialized algorithms */

template<typename SuperMeta,typename TagList>
void swap(
  random_access_index<SuperMeta,TagList>& x,
  random_access_index<SuperMeta,TagList>& y)
{
  x.swap(y);
}

} /* namespace multi_index::detail */

/* random access index specifier */

template <typename TagList>
struct random_access
{
  BOOST_STATIC_ASSERT(detail::is_tag<TagList>::value);

  template<typename Super>
  struct node_class
  {
    typedef detail::random_access_index_node<Super> type;
  };

  template<typename SuperMeta>
  struct index_class
  {
    typedef detail::random_access_index<
      SuperMeta,typename TagList::type>  type;
  };
};

} /* namespace multi_index */

} /* namespace boost */

#undef BOOST_MULTI_INDEX_RND_INDEX_CHECK_INVARIANT

#endif
