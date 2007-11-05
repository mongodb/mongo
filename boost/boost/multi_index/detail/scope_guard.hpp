/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_SCOPE_GUARD_HPP
#define BOOST_MULTI_INDEX_DETAIL_SCOPE_GUARD_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* Until some official version of the ScopeGuard idiom makes it into Boost,
 * we locally define our own. This is a merely reformated version of
 * ScopeGuard.h as defined in:
 *   Alexandrescu, A., Marginean, P.:"Generic<Programming>: Change the Way You
 *     Write Exception-Safe Code - Forever", C/C++ Users Jornal, Dec 2000,
 *     http://www.cuj.com/documents/s=8000/cujcexp1812alexandr/
 * with the following modifications:
 *   - General pretty formatting (pretty to my taste at least.)
 *   - Naming style changed to standard C++ library requirements.
 *   - safe_execute does not feature a try-catch protection, so we can
 *     use this even if BOOST_NO_EXCEPTIONS is defined.
 *   - Added scope_guard_impl4 and obj_scope_guard_impl3, (Boost.MultiIndex
 *     needs them). A better design would provide guards for many more
 *     arguments through the Boost Preprocessor Library.
 *   - Added scope_guard_impl_base::touch (see below.)
 *   - Removed RefHolder and ByRef, whose functionality is provided
 *     already by Boost.Ref.
 *   - Removed static make_guard's and make_obj_guard's, so that the code
 *     will work even if BOOST_NO_MEMBER_TEMPLATES is defined. This forces
 *     us to move some private ctors to public, though.
 *
 * NB: CodeWarrior Pro 8 seems to have problems looking up safe_execute
 * without an explicit qualification.
 */

class scope_guard_impl_base
{
public:
  scope_guard_impl_base():dismissed_(false){}
  void dismiss()const{dismissed_=true;}

  /* This helps prevent some "unused variable" warnings under, for instance,
   * GCC 3.2.
   */
  void touch()const{}

protected:
  ~scope_guard_impl_base(){}

  scope_guard_impl_base(const scope_guard_impl_base& other):
    dismissed_(other.dismissed_)
  {
    other.dismiss();
  }

  template<typename J>
  static void safe_execute(J& j){if(!j.dismissed_)j.execute();}
  
  mutable bool dismissed_;

private:
  scope_guard_impl_base& operator=(const scope_guard_impl_base&);
};

typedef const scope_guard_impl_base& scope_guard;

template<typename F>
class scope_guard_impl0:public scope_guard_impl_base
{
public:
  scope_guard_impl0(F fun):fun_(fun){}
  ~scope_guard_impl0(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){fun_();}

protected:

  F fun_;
};

template<typename F> 
inline scope_guard_impl0<F> make_guard(F fun)
{
  return scope_guard_impl0<F>(fun);
}

template<typename F,typename P1>
class scope_guard_impl1:public scope_guard_impl_base
{
public:
  scope_guard_impl1(F fun,P1 p1):fun_(fun),p1_(p1){}
  ~scope_guard_impl1(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){fun_(p1_);}

protected:
  F        fun_;
  const P1 p1_;
};

template<typename F,typename P1> 
inline scope_guard_impl1<F,P1> make_guard(F fun,P1 p1)
{
  return scope_guard_impl1<F,P1>(fun,p1);
}

template<typename F,typename P1,typename P2>
class scope_guard_impl2:public scope_guard_impl_base
{
public:
  scope_guard_impl2(F fun,P1 p1,P2 p2):fun_(fun),p1_(p1),p2_(p2){}
  ~scope_guard_impl2(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){fun_(p1_,p2_);}

protected:
  F        fun_;
  const P1 p1_;
  const P2 p2_;
};

template<typename F,typename P1,typename P2>
inline scope_guard_impl2<F,P1,P2> make_guard(F fun,P1 p1,P2 p2)
{
  return scope_guard_impl2<F,P1,P2>(fun,p1,p2);
}

template<typename F,typename P1,typename P2,typename P3>
class scope_guard_impl3:public scope_guard_impl_base
{
public:
  scope_guard_impl3(F fun,P1 p1,P2 p2,P3 p3):fun_(fun),p1_(p1),p2_(p2),p3_(p3){}
  ~scope_guard_impl3(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){fun_(p1_,p2_,p3_);}

protected:
  F        fun_;
  const P1 p1_;
  const P2 p2_;
  const P3 p3_;
};

template<typename F,typename P1,typename P2,typename P3>
inline scope_guard_impl3<F,P1,P2,P3> make_guard(F fun,P1 p1,P2 p2,P3 p3)
{
  return scope_guard_impl3<F,P1,P2,P3>(fun,p1,p2,p3);
}

template<typename F,typename P1,typename P2,typename P3,typename P4>
class scope_guard_impl4:public scope_guard_impl_base
{
public:
  scope_guard_impl4(F fun,P1 p1,P2 p2,P3 p3,P4 p4):
    fun_(fun),p1_(p1),p2_(p2),p3_(p3),p4_(p4){}
  ~scope_guard_impl4(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){fun_(p1_,p2_,p3_,p4_);}

protected:
  F        fun_;
  const P1 p1_;
  const P2 p2_;
  const P3 p3_;
  const P4 p4_;
};

template<typename F,typename P1,typename P2,typename P3,typename P4>
inline scope_guard_impl4<F,P1,P2,P3,P4> make_guard(
  F fun,P1 p1,P2 p2,P3 p3,P4 p4)
{
  return scope_guard_impl4<F,P1,P2,P3,P4>(fun,p1,p2,p3,p4);
}

template<class Obj,typename MemFun>
class obj_scope_guard_impl0:public scope_guard_impl_base
{
public:
  obj_scope_guard_impl0(Obj& obj,MemFun mem_fun):obj_(obj),mem_fun_(mem_fun){}
  ~obj_scope_guard_impl0(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){(obj_.*mem_fun_)();}

protected:
  Obj&   obj_;
  MemFun mem_fun_;
};

template<class Obj,typename MemFun>
inline obj_scope_guard_impl0<Obj,MemFun> make_obj_guard(Obj& obj,MemFun mem_fun)
{
  return obj_scope_guard_impl0<Obj,MemFun>(obj,mem_fun);
}

template<class Obj,typename MemFun,typename P1>
class obj_scope_guard_impl1:public scope_guard_impl_base
{
public:
  obj_scope_guard_impl1(Obj& obj,MemFun mem_fun,P1 p1):
    obj_(obj),mem_fun_(mem_fun),p1_(p1){}
  ~obj_scope_guard_impl1(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){(obj_.*mem_fun_)(p1_);}

protected:
  Obj&     obj_;
  MemFun   mem_fun_;
  const P1 p1_;
};

template<class Obj,typename MemFun,typename P1>
inline obj_scope_guard_impl1<Obj,MemFun,P1> make_obj_guard(
  Obj& obj,MemFun mem_fun,P1 p1)
{
  return obj_scope_guard_impl1<Obj,MemFun,P1>(obj,mem_fun,p1);
}

template<class Obj,typename MemFun,typename P1,typename P2>
class obj_scope_guard_impl2:public scope_guard_impl_base
{
public:
  obj_scope_guard_impl2(Obj& obj,MemFun mem_fun,P1 p1,P2 p2):
    obj_(obj),mem_fun_(mem_fun),p1_(p1),p2_(p2)
  {}
  ~obj_scope_guard_impl2(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){(obj_.*mem_fun_)(p1_,p2_);}

protected:
  Obj&     obj_;
  MemFun   mem_fun_;
  const P1 p1_;
  const P2 p2_;
};

template<class Obj,typename MemFun,typename P1,typename P2>
inline obj_scope_guard_impl2<Obj,MemFun,P1,P2>
make_obj_guard(Obj& obj,MemFun mem_fun,P1 p1,P2 p2)
{
  return obj_scope_guard_impl2<Obj,MemFun,P1,P2>(obj,mem_fun,p1,p2);
}

template<class Obj,typename MemFun,typename P1,typename P2,typename P3>
class obj_scope_guard_impl3:public scope_guard_impl_base
{
public:
  obj_scope_guard_impl3(Obj& obj,MemFun mem_fun,P1 p1,P2 p2,P3 p3):
    obj_(obj),mem_fun_(mem_fun),p1_(p1),p2_(p2),p3_(p3)
  {}
  ~obj_scope_guard_impl3(){scope_guard_impl_base::safe_execute(*this);}
  void execute(){(obj_.*mem_fun_)(p1_,p2_,p3_);}

protected:
  Obj&     obj_;
  MemFun   mem_fun_;
  const P1 p1_;
  const P2 p2_;
  const P3 p3_;
};

template<class Obj,typename MemFun,typename P1,typename P2,typename P3>
inline obj_scope_guard_impl3<Obj,MemFun,P1,P2,P3>
make_obj_guard(Obj& obj,MemFun mem_fun,P1 p1,P2 p2,P3 p3)
{
  return obj_scope_guard_impl3<Obj,MemFun,P1,P2,P3>(obj,mem_fun,p1,p2,p3);
}

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif
