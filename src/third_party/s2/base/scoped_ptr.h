// Copyright 2007 Google Inc.
// All Rights Reserved.
//
//
#ifndef BASE_SCOPED_PTR_H__
#define BASE_SCOPED_PTR_H__

//  This is an implementation designed to match the anticipated future TR2
//  implementation of the scoped_ptr class, and its closely-related brethren,
//  scoped_array, scoped_ptr_malloc, and make_scoped_ptr.
//
//  file.

#include <assert.h>
#include <stdlib.h>
#include <cstddef>

#ifdef OS_EMBEDDED_QNX
// NOTE(user):
// The C++ standard says that <stdlib.h> declares both ::foo and std::foo
// But this isn't done in QNX version 6.3.2 200709062316.
using std::free;
using std::malloc;
using std::realloc;
#endif

template <class C> class scoped_ptr;
template <class C, class Free> class scoped_ptr_malloc;
template <class C> class scoped_array;

template <class C>
scoped_ptr<C> make_scoped_ptr(C *);

// A scoped_ptr<T> is like a T*, except that the destructor of scoped_ptr<T>
// automatically deletes the pointer it holds (if any).
// That is, scoped_ptr<T> owns the T object that it points to.
// Like a T*, a scoped_ptr<T> may hold either NULL or a pointer to a T object.
// Also like T*, scoped_ptr<T> is thread-compatible, and once you
// dereference it, you get the threadsafety guarantees of T.
//
// The size of a scoped_ptr is small:
// sizeof(scoped_ptr<C>) == sizeof(C*)
template <class C>
class scoped_ptr {
 public:

  // The element type
  typedef C element_type;

  // Constructor.  Defaults to intializing with NULL.
  // There is no way to create an uninitialized scoped_ptr.
  // The input parameter must be allocated with new.
  explicit scoped_ptr(C* p = NULL) : ptr_(p) { }

  // Destructor.  If there is a C object, delete it.
  // We don't need to test ptr_ == NULL because C++ does that for us.
  ~scoped_ptr() {
    enum { type_must_be_complete = sizeof(C) };
    delete ptr_;
  }

  // Reset.  Deletes the current owned object, if any.
  // Then takes ownership of a new object, if given.
  // this->reset(this->get()) works.
  void reset(C* p = NULL) {
    if (p != ptr_) {
      enum { type_must_be_complete = sizeof(C) };
      delete ptr_;
      ptr_ = p;
    }
  }

  // Accessors to get the owned object.
  // operator* and operator-> will assert() if there is no current object.
  C& operator*() const {
    assert(ptr_ != NULL);
    return *ptr_;
  }
  C* operator->() const  {
    assert(ptr_ != NULL);
    return ptr_;
  }
  C* get() const { return ptr_; }

  // Comparison operators.
  // These return whether a scoped_ptr and a raw pointer refer to
  // the same object, not just to two different but equal objects.
  bool operator==(const C* p) const { return ptr_ == p; }
  bool operator!=(const C* p) const { return ptr_ != p; }

  // Swap two scoped pointers.
  void swap(scoped_ptr& p2) {
    C* tmp = ptr_;
    ptr_ = p2.ptr_;
    p2.ptr_ = tmp;
  }

  // Release a pointer.
  // The return value is the current pointer held by this object.
  // If this object holds a NULL pointer, the return value is NULL.
  // After this operation, this object will hold a NULL pointer,
  // and will not own the object any more.
  C* release() {
    C* retVal = ptr_;
    ptr_ = NULL;
    return retVal;
  }

 private:
  C* ptr_;

  // google3 friend class that can access copy ctor (although if it actually
  // calls a copy ctor, there will be a problem) see below
  friend scoped_ptr<C> make_scoped_ptr<C>(C *p);

  // Forbid comparison of scoped_ptr types.  If C2 != C, it totally doesn't
  // make sense, and if C2 == C, it still doesn't make sense because you should
  // never have the same object owned by two different scoped_ptrs.
  template <class C2> bool operator==(scoped_ptr<C2> const& p2) const;
  template <class C2> bool operator!=(scoped_ptr<C2> const& p2) const;

  // Disallow evil constructors
  scoped_ptr(const scoped_ptr&);
  void operator=(const scoped_ptr&);
};

// Free functions
template <class C>
inline void swap(scoped_ptr<C>& p1, scoped_ptr<C>& p2) {
  p1.swap(p2);
}

template <class C>
inline bool operator==(const C* p1, const scoped_ptr<C>& p2) {
  return p1 == p2.get();
}

template <class C>
inline bool operator==(const C* p1, const scoped_ptr<const C>& p2) {
  return p1 == p2.get();
}

template <class C>
inline bool operator!=(const C* p1, const scoped_ptr<C>& p2) {
  return p1 != p2.get();
}

template <class C>
inline bool operator!=(const C* p1, const scoped_ptr<const C>& p2) {
  return p1 != p2.get();
}

template <class C>
scoped_ptr<C> make_scoped_ptr(C *p) {
  // This does nothing but to return a scoped_ptr of the type that the passed
  // pointer is of.  (This eliminates the need to specify the name of T when
  // making a scoped_ptr that is used anonymously/temporarily.)  From an
  // access control point of view, we construct an unnamed scoped_ptr here
  // which we return and thus copy-construct.  Hence, we need to have access
  // to scoped_ptr::scoped_ptr(scoped_ptr const &).  However, it is guaranteed
  // that we never actually call the copy constructor, which is a good thing
  // as we would call the temporary's object destructor (and thus delete p)
  // if we actually did copy some object, here.
  return scoped_ptr<C>(p);
}

// scoped_array<C> is like scoped_ptr<C>, except that the caller must allocate
// with new [] and the destructor deletes objects with delete [].
//
// As with scoped_ptr<C>, a scoped_array<C> either points to an object
// or is NULL.  A scoped_array<C> owns the object that it points to.
// scoped_array<T> is thread-compatible, and once you index into it,
// the returned objects have only the threadsafety guarantees of T.
//
// Size: sizeof(scoped_array<C>) == sizeof(C*)
template <class C>
class scoped_array {
 public:

  // The element type
  typedef C element_type;

  // Constructor.  Defaults to intializing with NULL.
  // There is no way to create an uninitialized scoped_array.
  // The input parameter must be allocated with new [].
  explicit scoped_array(C* p = NULL) : array_(p) { }

  // Destructor.  If there is a C object, delete it.
  // We don't need to test ptr_ == NULL because C++ does that for us.
  ~scoped_array() {
    enum { type_must_be_complete = sizeof(C) };
    delete[] array_;
  }

  // Reset.  Deletes the current owned object, if any.
  // Then takes ownership of a new object, if given.
  // this->reset(this->get()) works.
  void reset(C* p = NULL) {
    if (p != array_) {
      enum { type_must_be_complete = sizeof(C) };
      delete[] array_;
      array_ = p;
    }
  }

  // Get one element of the current object.
  // Will assert() if there is no current object, or index i is negative.
  C& operator[](std::ptrdiff_t i) const {
    assert(i >= 0);
    assert(array_ != NULL);
    return array_[i];
  }

  // Get a pointer to the zeroth element of the current object.
  // If there is no current object, return NULL.
  C* get() const {
    return array_;
  }

  // Comparison operators.
  // These return whether a scoped_array and a raw pointer refer to
  // the same array, not just to two different but equal arrays.
  bool operator==(const C* p) const { return array_ == p; }
  bool operator!=(const C* p) const { return array_ != p; }

  // Swap two scoped arrays.
  void swap(scoped_array& p2) {
    C* tmp = array_;
    array_ = p2.array_;
    p2.array_ = tmp;
  }

  // Release an array.
  // The return value is the current pointer held by this object.
  // If this object holds a NULL pointer, the return value is NULL.
  // After this operation, this object will hold a NULL pointer,
  // and will not own the object any more.
  C* release() {
    C* retVal = array_;
    array_ = NULL;
    return retVal;
  }

 private:
  C* array_;

  // Forbid comparison of different scoped_array types.
  template <class C2> bool operator==(scoped_array<C2> const& p2) const;
  template <class C2> bool operator!=(scoped_array<C2> const& p2) const;

  // Disallow evil constructors
  scoped_array(const scoped_array&);
  void operator=(const scoped_array&);
};

// Free functions
template <class C>
inline void swap(scoped_array<C>& p1, scoped_array<C>& p2) {
  p1.swap(p2);
}

template <class C>
inline bool operator==(const C* p1, const scoped_array<C>& p2) {
  return p1 == p2.get();
}

template <class C>
inline bool operator==(const C* p1, const scoped_array<const C>& p2) {
  return p1 == p2.get();
}

template <class C>
inline bool operator!=(const C* p1, const scoped_array<C>& p2) {
  return p1 != p2.get();
}

template <class C>
inline bool operator!=(const C* p1, const scoped_array<const C>& p2) {
  return p1 != p2.get();
}

// This class wraps the c library function free() in a class that can be
// passed as a template argument to scoped_ptr_malloc below.
class ScopedPtrMallocFree {
 public:
  inline void operator()(void* x) const {
    free(x);
  }
};

// scoped_ptr_malloc<> is similar to scoped_ptr<>, but it accepts a
// second template argument, the functor used to free the object.

template<class C, class FreeProc = ScopedPtrMallocFree>
class scoped_ptr_malloc {
 public:

  // The element type
  typedef C element_type;

  // Construction with no arguments sets ptr_ to NULL.
  // There is no way to create an uninitialized scoped_ptr.
  // The input parameter must be allocated with an allocator that matches the
  // Free functor.  For the default Free functor, this is malloc, calloc, or
  // realloc.
  explicit scoped_ptr_malloc(): ptr_(NULL) { }

  // Construct with a C*, and provides an error with a D*.
  template<class must_be_C>
  explicit scoped_ptr_malloc(must_be_C* p): ptr_(p) { }

  // Construct with a void*, such as you get from malloc.
  explicit scoped_ptr_malloc(void *p): ptr_(static_cast<C*>(p)) { }

  // Destructor.  If there is a C object, call the Free functor.
  ~scoped_ptr_malloc() {
    free_(ptr_);
  }

  // Reset.  Calls the Free functor on the current owned object, if any.
  // Then takes ownership of a new object, if given.
  // this->reset(this->get()) works.
  void reset(C* p = NULL) {
    if (ptr_ != p) {
      free_(ptr_);
      ptr_ = p;
    }
  }

  // Reallocates the existing pointer, and returns 'true' if
  // the reallcation is succesfull.  If the reallocation failed, then
  // the pointer remains in its previous state.
  //
  // Note: this calls realloc() directly, even if an alternate 'free'
  // functor is provided in the template instantiation.
  bool try_realloc(size_t new_size) {
    C* new_ptr = static_cast<C*>(realloc(ptr_, new_size));
    if (new_ptr == NULL) {
      return false;
    }
    ptr_ = new_ptr;
    return true;
  }

  // Get the current object.
  // operator* and operator-> will cause an assert() failure if there is
  // no current object.
  C& operator*() const {
    assert(ptr_ != NULL);
    return *ptr_;
  }

  C* operator->() const {
    assert(ptr_ != NULL);
    return ptr_;
  }

  C* get() const {
    return ptr_;
  }

  // Comparison operators.
  // These return whether a scoped_ptr_malloc and a plain pointer refer
  // to the same object, not just to two different but equal objects.
  // For compatibility with the boost-derived implementation, these
  // take non-const arguments.
  bool operator==(C* p) const {
    return ptr_ == p;
  }

  bool operator!=(C* p) const {
    return ptr_ != p;
  }

  // Swap two scoped pointers.
  void swap(scoped_ptr_malloc & b) {
    C* tmp = b.ptr_;
    b.ptr_ = ptr_;
    ptr_ = tmp;
  }

  // Release a pointer.
  // The return value is the current pointer held by this object.
  // If this object holds a NULL pointer, the return value is NULL.
  // After this operation, this object will hold a NULL pointer,
  // and will not own the object any more.
  C* release() {
    C* tmp = ptr_;
    ptr_ = NULL;
    return tmp;
  }

 private:
  C* ptr_;

  // no reason to use these: each scoped_ptr_malloc should have its own object
  template <class C2, class GP>
  bool operator==(scoped_ptr_malloc<C2, GP> const& p) const;
  template <class C2, class GP>
  bool operator!=(scoped_ptr_malloc<C2, GP> const& p) const;

  static FreeProc const free_;

  // Disallow evil constructors
  scoped_ptr_malloc(const scoped_ptr_malloc&);
  void operator=(const scoped_ptr_malloc&);
};

template<class C, class FP>
FP const scoped_ptr_malloc<C, FP>::free_ = FP();

template<class C, class FP> inline
void swap(scoped_ptr_malloc<C, FP>& a, scoped_ptr_malloc<C, FP>& b) {
  a.swap(b);
}

template<class C, class FP> inline
bool operator==(C* p, const scoped_ptr_malloc<C, FP>& b) {
  return p == b.get();
}

template<class C, class FP> inline
bool operator!=(C* p, const scoped_ptr_malloc<C, FP>& b) {
  return p != b.get();
}

#endif  // BASE_SCOPED_PTR_H__
