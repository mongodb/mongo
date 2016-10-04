////////////////////////////////////////////////////////////////////////////////
// The Loki Library
// Copyright (c) 2000 Andrei Alexandrescu
// Copyright (c) 2000 Petru Marginean
// Copyright (c) 2005 Joshua Lehrer
//
// Permission to use, copy, modify, distribute and sell this software for any
//     purpose is hereby granted without fee, provided that the above copyright
//     notice appear in all copies and that both that copyright notice and this
//     permission notice appear in supporting documentation.
// The author makes no representations about the
//     suitability of this software for any purpose. It is provided "as is"
//     without express or implied warranty.
////////////////////////////////////////////////////////////////////////////////
#ifndef LOKI_SCOPEGUARD_H_
#define LOKI_SCOPEGUARD_H_

#include "mongo/platform/compiler.h"

namespace mongo {

////////////////////////////////////////////////////////////////////////////////
///  \class RefToValue
///
///  Transports a reference as a value
///  Serves to implement the Colvin/Gibbons trick for SmartPtr/ScopeGuard
////////////////////////////////////////////////////////////////////////////////

template <class T>
class RefToValue {
public:
    RefToValue(T& ref) : ref_(ref) {}

    RefToValue(const RefToValue& rhs) : ref_(rhs.ref_) {}

    operator T&() const {
        return ref_;
    }

private:
    // Disable - not implemented
    RefToValue();
    RefToValue& operator=(const RefToValue&);

    T& ref_;
};


////////////////////////////////////////////////////////////////////////////////
///  RefToValue creator.
////////////////////////////////////////////////////////////////////////////////

template <class T>
inline RefToValue<T> ByRef(T& t) {
    return RefToValue<T>(t);
}


////////////////////////////////////////////
///  ScopeGuard
/*
  Trivial example for use:

  FILE* f = fopen("myfile.txt", "w+");
  if (!f)
      return error;
  ON_BLOCK_EXIT(fclose, f);


  More complicated example:

  ScopeGuard guard = MakeGuard(my_rollback_func, myparam);
  ...
  if (successful) {
     guard.Dismiss();
     return;
  }
  // guard is still active here and will fire at scope exit
  ...


*/


class ScopeGuardImplBase {
    ScopeGuardImplBase& operator=(const ScopeGuardImplBase&);

protected:
    ~ScopeGuardImplBase() {}

    ScopeGuardImplBase(const ScopeGuardImplBase& other) throw() : dismissed_(other.dismissed_) {
        other.Dismiss();
    }

    template <typename J>
    static void SafeExecute(J& j) throw() {
        if (!j.dismissed_)
            try {
                j.Execute();
            } catch (...) {
            }
    }

    mutable bool dismissed_;

public:
    ScopeGuardImplBase() throw() : dismissed_(false) {}

    void Dismiss() const throw() {
        dismissed_ = true;
    }
};

////////////////////////////////////////////////////////////////
///
/// \typedef typedef const ScopeGuardImplBase& ScopeGuard
///
/// See Andrei's and Petru Marginean's CUJ article
/// http://www.cuj.com/documents/s=8000/cujcexp1812alexandr/alexandr.htm
///
/// Changes to the original code by Joshua Lehrer:
/// http://www.lehrerfamily.com/scopeguard.html
////////////////////////////////////////////////////////////////

typedef const ScopeGuardImplBase& ScopeGuard;

template <typename F>
class ScopeGuardImpl0 : public ScopeGuardImplBase {
public:
    static ScopeGuardImpl0<F> MakeGuard(F fun) {
        return ScopeGuardImpl0<F>(fun);
    }

    ~ScopeGuardImpl0() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        fun_();
    }

protected:
    ScopeGuardImpl0(F fun) : fun_(fun) {}

    F fun_;
};

template <typename F>
inline ScopeGuardImpl0<F> MakeGuard(F fun) {
    return ScopeGuardImpl0<F>::MakeGuard(fun);
}

template <typename F, typename P1>
class ScopeGuardImpl1 : public ScopeGuardImplBase {
public:
    static ScopeGuardImpl1<F, P1> MakeGuard(F fun, P1 p1) {
        return ScopeGuardImpl1<F, P1>(fun, p1);
    }

    ~ScopeGuardImpl1() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        fun_(p1_);
    }

protected:
    ScopeGuardImpl1(F fun, P1 p1) : fun_(fun), p1_(p1) {}

    F fun_;
    const P1 p1_;
};

template <typename F, typename P1>
inline ScopeGuardImpl1<F, P1> MakeGuard(F fun, P1 p1) {
    return ScopeGuardImpl1<F, P1>::MakeGuard(fun, p1);
}

template <typename F, typename P1, typename P2>
class ScopeGuardImpl2 : public ScopeGuardImplBase {
public:
    static ScopeGuardImpl2<F, P1, P2> MakeGuard(F fun, P1 p1, P2 p2) {
        return ScopeGuardImpl2<F, P1, P2>(fun, p1, p2);
    }

    ~ScopeGuardImpl2() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        fun_(p1_, p2_);
    }

protected:
    ScopeGuardImpl2(F fun, P1 p1, P2 p2) : fun_(fun), p1_(p1), p2_(p2) {}

    F fun_;
    const P1 p1_;
    const P2 p2_;
};

template <typename F, typename P1, typename P2>
inline ScopeGuardImpl2<F, P1, P2> MakeGuard(F fun, P1 p1, P2 p2) {
    return ScopeGuardImpl2<F, P1, P2>::MakeGuard(fun, p1, p2);
}

template <typename F, typename P1, typename P2, typename P3>
class ScopeGuardImpl3 : public ScopeGuardImplBase {
public:
    static ScopeGuardImpl3<F, P1, P2, P3> MakeGuard(F fun, P1 p1, P2 p2, P3 p3) {
        return ScopeGuardImpl3<F, P1, P2, P3>(fun, p1, p2, p3);
    }

    ~ScopeGuardImpl3() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        fun_(p1_, p2_, p3_);
    }

protected:
    ScopeGuardImpl3(F fun, P1 p1, P2 p2, P3 p3) : fun_(fun), p1_(p1), p2_(p2), p3_(p3) {}

    F fun_;
    const P1 p1_;
    const P2 p2_;
    const P3 p3_;
};

template <typename F, typename P1, typename P2, typename P3>
inline ScopeGuardImpl3<F, P1, P2, P3> MakeGuard(F fun, P1 p1, P2 p2, P3 p3) {
    return ScopeGuardImpl3<F, P1, P2, P3>::MakeGuard(fun, p1, p2, p3);
}

//************************************************************

template <class Obj, typename MemFun>
class ObjScopeGuardImpl0 : public ScopeGuardImplBase {
public:
    static ObjScopeGuardImpl0<Obj, MemFun> MakeObjGuard(Obj& obj, MemFun memFun) {
        return ObjScopeGuardImpl0<Obj, MemFun>(obj, memFun);
    }

    ~ObjScopeGuardImpl0() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        (obj_.*memFun_)();
    }

protected:
    ObjScopeGuardImpl0(Obj& obj, MemFun memFun) : obj_(obj), memFun_(memFun) {}

    Obj& obj_;
    MemFun memFun_;
};

template <class Obj, typename MemFun>
inline ObjScopeGuardImpl0<Obj, MemFun> MakeObjGuard(Obj& obj, MemFun memFun) {
    return ObjScopeGuardImpl0<Obj, MemFun>::MakeObjGuard(obj, memFun);
}

template <typename Ret, class Obj1, class Obj2>
inline ObjScopeGuardImpl0<Obj1, Ret (Obj2::*)()> MakeGuard(Ret (Obj2::*memFun)(), Obj1& obj) {
    return ObjScopeGuardImpl0<Obj1, Ret (Obj2::*)()>::MakeObjGuard(obj, memFun);
}

template <typename Ret, class Obj1, class Obj2>
inline ObjScopeGuardImpl0<Obj1, Ret (Obj2::*)()> MakeGuard(Ret (Obj2::*memFun)(), Obj1* obj) {
    return ObjScopeGuardImpl0<Obj1, Ret (Obj2::*)()>::MakeObjGuard(*obj, memFun);
}

template <class Obj, typename MemFun, typename P1>
class ObjScopeGuardImpl1 : public ScopeGuardImplBase {
public:
    static ObjScopeGuardImpl1<Obj, MemFun, P1> MakeObjGuard(Obj& obj, MemFun memFun, P1 p1) {
        return ObjScopeGuardImpl1<Obj, MemFun, P1>(obj, memFun, p1);
    }

    ~ObjScopeGuardImpl1() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        (obj_.*memFun_)(p1_);
    }

protected:
    ObjScopeGuardImpl1(Obj& obj, MemFun memFun, P1 p1) : obj_(obj), memFun_(memFun), p1_(p1) {}

    Obj& obj_;
    MemFun memFun_;
    const P1 p1_;
};

template <class Obj, typename MemFun, typename P1>
inline ObjScopeGuardImpl1<Obj, MemFun, P1> MakeObjGuard(Obj& obj, MemFun memFun, P1 p1) {
    return ObjScopeGuardImpl1<Obj, MemFun, P1>::MakeObjGuard(obj, memFun, p1);
}

template <typename Ret, class Obj1, class Obj2, typename P1a, typename P1b>
inline ObjScopeGuardImpl1<Obj1, Ret (Obj2::*)(P1a), P1b> MakeGuard(Ret (Obj2::*memFun)(P1a),
                                                                   Obj1& obj,
                                                                   P1b p1) {
    return ObjScopeGuardImpl1<Obj1, Ret (Obj2::*)(P1a), P1b>::MakeObjGuard(obj, memFun, p1);
}

template <typename Ret, class Obj1, class Obj2, typename P1a, typename P1b>
inline ObjScopeGuardImpl1<Obj1, Ret (Obj2::*)(P1a), P1b> MakeGuard(Ret (Obj2::*memFun)(P1a),
                                                                   Obj1* obj,
                                                                   P1b p1) {
    return ObjScopeGuardImpl1<Obj1, Ret (Obj2::*)(P1a), P1b>::MakeObjGuard(*obj, memFun, p1);
}

template <class Obj, typename MemFun, typename P1, typename P2>
class ObjScopeGuardImpl2 : public ScopeGuardImplBase {
public:
    static ObjScopeGuardImpl2<Obj, MemFun, P1, P2> MakeObjGuard(Obj& obj,
                                                                MemFun memFun,
                                                                P1 p1,
                                                                P2 p2) {
        return ObjScopeGuardImpl2<Obj, MemFun, P1, P2>(obj, memFun, p1, p2);
    }

    ~ObjScopeGuardImpl2() throw() {
        SafeExecute(*this);
    }

    void Execute() {
        (obj_.*memFun_)(p1_, p2_);
    }

protected:
    ObjScopeGuardImpl2(Obj& obj, MemFun memFun, P1 p1, P2 p2)
        : obj_(obj), memFun_(memFun), p1_(p1), p2_(p2) {}

    Obj& obj_;
    MemFun memFun_;
    const P1 p1_;
    const P2 p2_;
};

template <class Obj, typename MemFun, typename P1, typename P2>
inline ObjScopeGuardImpl2<Obj, MemFun, P1, P2> MakeObjGuard(Obj& obj, MemFun memFun, P1 p1, P2 p2) {
    return ObjScopeGuardImpl2<Obj, MemFun, P1, P2>::MakeObjGuard(obj, memFun, p1, p2);
}

template <typename Ret,
          class Obj1,
          class Obj2,
          typename P1a,
          typename P1b,
          typename P2a,
          typename P2b>
inline ObjScopeGuardImpl2<Obj1, Ret (Obj2::*)(P1a, P2a), P1b, P2b> MakeGuard(
    Ret (Obj2::*memFun)(P1a, P2a), Obj1& obj, P1b p1, P2b p2) {
    return ObjScopeGuardImpl2<Obj1, Ret (Obj2::*)(P1a, P2a), P1b, P2b>::MakeObjGuard(
        obj, memFun, p1, p2);
}

template <typename Ret,
          class Obj1,
          class Obj2,
          typename P1a,
          typename P1b,
          typename P2a,
          typename P2b>
inline ObjScopeGuardImpl2<Obj1, Ret (Obj2::*)(P1a, P2a), P1b, P2b> MakeGuard(
    Ret (Obj2::*memFun)(P1a, P2a), Obj1* obj, P1b p1, P2b p2) {
    return ObjScopeGuardImpl2<Obj1, Ret (Obj2::*)(P1a, P2a), P1b, P2b>::MakeObjGuard(
        *obj, memFun, p1, p2);
}

}  // namespace Loki

#define LOKI_CONCATENATE_DIRECT(s1, s2) s1##s2
#define LOKI_CONCATENATE(s1, s2) LOKI_CONCATENATE_DIRECT(s1, s2)
#define LOKI_ANONYMOUS_VARIABLE(str) LOKI_CONCATENATE(str, __LINE__)

#define ON_BLOCK_EXIT \
    MONGO_COMPILER_VARIABLE_UNUSED ScopeGuard LOKI_ANONYMOUS_VARIABLE(scopeGuard) = MakeGuard
#define ON_BLOCK_EXIT_OBJ \
    MONGO_COMPILER_VARIABLE_UNUSED ScopeGuard LOKI_ANONYMOUS_VARIABLE(scopeGuard) = MakeObjGuard

#endif  // LOKI_SCOPEGUARD_H_
