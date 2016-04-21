/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* C++11-style, but C++98-usable, "move references" implementation. */

#ifndef mozilla_Move_h
#define mozilla_Move_h

#include "mozilla/TypeTraits.h"

namespace mozilla {

/*
 * "Move" References
 *
 * Some types can be copied much more efficiently if we know the original's
 * value need not be preserved --- that is, if we are doing a "move", not a
 * "copy". For example, if we have:
 *
 *   Vector<T> u;
 *   Vector<T> v(u);
 *
 * the constructor for v must apply a copy constructor to each element of u ---
 * taking time linear in the length of u. However, if we know we will not need u
 * any more once v has been initialized, then we could initialize v very
 * efficiently simply by stealing u's dynamically allocated buffer and giving it
 * to v --- a constant-time operation, regardless of the size of u.
 *
 * Moves often appear in container implementations. For example, when we append
 * to a vector, we may need to resize its buffer. This entails moving each of
 * its extant elements from the old, smaller buffer to the new, larger buffer.
 * But once the elements have been migrated, we're just going to throw away the
 * old buffer; we don't care if they still have their values. So if the vector's
 * element type can implement "move" more efficiently than "copy", the vector
 * resizing should by all means use a "move" operation. Hash tables should also
 * use moves when resizing their internal array as entries are added and
 * removed.
 *
 * The details of the optimization, and whether it's worth applying, vary
 * from one type to the next: copying an 'int' is as cheap as moving it, so
 * there's no benefit in distinguishing 'int' moves from copies. And while
 * some constructor calls for complex types are moves, many really have to
 * be copies, and can't be optimized this way. So we need:
 *
 * 1) a way for a type (like Vector) to announce that it can be moved more
 *    efficiently than it can be copied, and provide an implementation of that
 *    move operation; and
 *
 * 2) a way for a particular invocation of a copy constructor to say that it's
 *    really a move, not a copy, and that the value of the original isn't
 *    important afterwards (although it must still be safe to destroy).
 *
 * If a constructor has a single argument of type 'T&&' (an 'rvalue reference
 * to T'), that indicates that it is a 'move constructor'. That's 1). It should
 * move, not copy, its argument into the object being constructed. It may leave
 * the original in any safely-destructible state.
 *
 * If a constructor's argument is an rvalue, as in 'C(f(x))' or 'C(x + y)', as
 * opposed to an lvalue, as in 'C(x)', then overload resolution will prefer the
 * move constructor, if there is one. The 'mozilla::Move' function, defined in
 * this file, is an identity function you can use in a constructor invocation to
 * make any argument into an rvalue, like this: C(Move(x)). That's 2). (You
 * could use any function that works, but 'Move' indicates your intention
 * clearly.)
 *
 * Where we might define a copy constructor for a class C like this:
 *
 *   C(const C& rhs) { ... copy rhs to this ... }
 *
 * we would declare a move constructor like this:
 *
 *   C(C&& rhs) { .. move rhs to this ... }
 *
 * And where we might perform a copy like this:
 *
 *   C c2(c1);
 *
 * we would perform a move like this:
 *
 *   C c2(Move(c1));
 *
 * Note that 'T&&' implicitly converts to 'T&'. So you can pass a 'T&&' to an
 * ordinary copy constructor for a type that doesn't support a special move
 * constructor, and you'll just get a copy. This means that templates can use
 * Move whenever they know they won't use the original value any more, even if
 * they're not sure whether the type at hand has a specialized move constructor.
 * If it doesn't, the 'T&&' will just convert to a 'T&', and the ordinary copy
 * constructor will apply.
 *
 * A class with a move constructor can also provide a move assignment operator.
 * A generic definition would run this's destructor, and then apply the move
 * constructor to *this's memory. A typical definition:
 *
 *   C& operator=(C&& rhs) {
 *     MOZ_ASSERT(&rhs != this, "self-moves are prohibited");
 *     this->~C();
 *     new(this) C(Move(rhs));
 *     return *this;
 *   }
 *
 * With that in place, one can write move assignments like this:
 *
 *   c2 = Move(c1);
 *
 * This destroys c2, moves c1's value to c2, and leaves c1 in an undefined but
 * destructible state.
 *
 * As we say, a move must leave the original in a "destructible" state. The
 * original's destructor will still be called, so if a move doesn't
 * actually steal all its resources, that's fine. We require only that the
 * move destination must take on the original's value; and that destructing
 * the original must not break the move destination.
 *
 * (Opinions differ on whether move assignment operators should deal with move
 * assignment of an object onto itself. It seems wise to either handle that
 * case, or assert that it does not occur.)
 *
 * Forwarding:
 *
 * Sometimes we want copy construction or assignment if we're passed an ordinary
 * value, but move construction if passed an rvalue reference. For example, if
 * our constructor takes two arguments and either could usefully be a move, it
 * seems silly to write out all four combinations:
 *
 *   C::C(X&  x, Y&  y) : x(x),       y(y)       { }
 *   C::C(X&  x, Y&& y) : x(x),       y(Move(y)) { }
 *   C::C(X&& x, Y&  y) : x(Move(x)), y(y)       { }
 *   C::C(X&& x, Y&& y) : x(Move(x)), y(Move(y)) { }
 *
 * To avoid this, C++11 has tweaks to make it possible to write what you mean.
 * The four constructor overloads above can be written as one constructor
 * template like so[0]:
 *
 *   template <typename XArg, typename YArg>
 *   C::C(XArg&& x, YArg&& y) : x(Forward<XArg>(x)), y(Forward<YArg>(y)) { }
 *
 * ("'Don't Repeat Yourself'? What's that?")
 *
 * This takes advantage of two new rules in C++11:
 *
 * - First, when a function template takes an argument that is an rvalue
 *   reference to a template argument (like 'XArg&& x' and 'YArg&& y' above),
 *   then when the argument is applied to an lvalue, the template argument
 *   resolves to 'T&'; and when it is applied to an rvalue, the template
 *   argument resolves to 'T'. Thus, in a call to C::C like:
 *
 *      X foo(int);
 *      Y yy;
 *
 *      C(foo(5), yy)
 *
 *   XArg would resolve to 'X', and YArg would resolve to 'Y&'.
 *
 * - Second, Whereas C++ used to forbid references to references, C++11 defines
 *   'collapsing rules': 'T& &', 'T&& &', and 'T& &&' (that is, any combination
 *   involving an lvalue reference) now collapse to simply 'T&'; and 'T&& &&'
 *   collapses to 'T&&'.
 *
 *   Thus, in the call above, 'XArg&&' is 'X&&'; and 'YArg&&' is 'Y& &&', which
 *   collapses to 'Y&'. Because the arguments are declared as rvalue references
 *   to template arguments, the lvalue-ness "shines through" where present.
 *
 * Then, the 'Forward<T>' function --- you must invoke 'Forward' with its type
 * argument --- returns an lvalue reference or an rvalue reference to its
 * argument, depending on what T is. In our unified constructor definition, that
 * means that we'll invoke either the copy or move constructors for x and y,
 * depending on what we gave C's constructor. In our call, we'll move 'foo()'
 * into 'x', but copy 'yy' into 'y'.
 *
 * This header file defines Move and Forward in the mozilla namespace. It's up
 * to individual containers to annotate moves as such, by calling Move; and it's
 * up to individual types to define move constructors and assignment operators
 * when valuable.
 *
 * (C++11 says that the <utility> header file should define 'std::move' and
 * 'std::forward', which are just like our 'Move' and 'Forward'; but those
 * definitions aren't available in that header on all our platforms, so we
 * define them ourselves here.)
 *
 * 0. This pattern is known as "perfect forwarding".  Interestingly, it is not
 *    actually perfect, and it can't forward all possible argument expressions!
 *    There is a C++11 issue: you can't form a reference to a bit-field.  As a
 *    workaround, assign the bit-field to a local variable and use that:
 *
 *      // C is as above
 *      struct S { int x : 1; } s;
 *      C(s.x, 0); // BAD: s.x is a reference to a bit-field, can't form those
 *      int tmp = s.x;
 *      C(tmp, 0); // OK: tmp not a bit-field
 */

/**
 * Identical to std::Move(); this is necessary until our stlport supports
 * std::move().
 */
template<typename T>
inline typename RemoveReference<T>::Type&&
Move(T&& aX)
{
  return static_cast<typename RemoveReference<T>::Type&&>(aX);
}

/**
 * These two overloads are identical to std::forward(); they are necessary until
 * our stlport supports std::forward().
 */
template<typename T>
inline T&&
Forward(typename RemoveReference<T>::Type& aX)
{
  return static_cast<T&&>(aX);
}

template<typename T>
inline T&&
Forward(typename RemoveReference<T>::Type&& aX)
{
  static_assert(!IsLvalueReference<T>::value,
                "misuse of Forward detected!  try the other overload");
  return static_cast<T&&>(aX);
}

/** Swap |aX| and |aY| using move-construction if possible. */
template<typename T>
inline void
Swap(T& aX, T& aY)
{
  T tmp(Move(aX));
  aX = Move(aY);
  aY = Move(tmp);
}

} // namespace mozilla

#endif /* mozilla_Move_h */
