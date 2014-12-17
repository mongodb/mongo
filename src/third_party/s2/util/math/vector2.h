// Copyright 2003 Google, Inc.
// All Rights Reserved.
//
//
// A simple class to handle vectors in 2D
// See the vector2-inl.h file for more details


#ifndef UTIL_MATH_VECTOR2_H__
#define UTIL_MATH_VECTOR2_H__

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;
  // NOLINT(readability/streams)
#include "base/basictypes.h"

template <typename VType> class Vector2;

// TODO(user): Look into creating conversion operators to remove the
// need to forward-declare Vector3 and Vector4.
template <typename VType> class Vector3;
template <typename VType> class Vector4;

// Template class for 2D vectors.
// All definitions for these functions are in vector2-inl.h.  That header will
// need to be included in order to actually use this class.  This class can be
// regarded to only forward-declare Vector2.
template <typename VType>
class Vector2 {
 private:
  VType c_[2];

  // FloatType is the type returned by Norm() and Angle().  These methods are
  // special because they return floating-point values even when VType is an
  // integer.
  typedef typename base::if_<base::is_integral<VType>::value,
                             double, VType>::type FloatType;

 public:
  typedef Vector2<VType> Self;
  typedef VType BaseType;
  // Create a new vector (0,0)
  Vector2();
  // Create a new vector (x,y)
  Vector2(const VType x, const VType y);
  // Create a new copy of the vector vb
  Vector2(const Self &vb);  // NOLINT(runtime/explicit)
  // Keep only the two first coordinates of the vector vb
  explicit Vector2(const Vector3<VType> &vb);
  // Keep only the two first coordinates of the vector vb
  explicit Vector2(const Vector4<VType> &vb);
  // Convert from another vector type
  template <typename VType2>
  static Self Cast(const Vector2<VType2> &vb);
  // Return the size of the vector
  static int Size() { return 2; }
  // Modify the coordinates of the current vector
  void Set(const VType x, const VType y);
  const Self& operator=(const Self &vb);
  // Add two vectors, component by component
  Self& operator+=(const Self &vb);
  // Subtract two vectors, component by component
  Self& operator-=(const Self &vb);
  // Multiply a vector by a scalar
  Self& operator*=(const VType k);
  // Divide a vector by a scalar
  Self& operator/=(const VType k);
  // Multiply two vectors component by component
  Self MulComponents(const Self &vb) const;
  // Divide two vectors component by component
  Self DivComponents(const Self &vb) const;
  // Add two vectors, component by component
  Self operator+(const Self &vb) const;
  // Subtract two vectors, component by component
  Self operator-(const Self &vb) const;
  // Change the sign of the components of a vector
  Self operator-() const;
  // Dot product.  Be aware that if VType is an integer type, the high bits of
  // the result are silently discarded.
  VType DotProd(const Self &vb) const;
  // Multiplication by a scalar
  Self operator*(const VType k) const;
  // Division by a scalar
  Self operator/(const VType k) const;
  // Cross product.  Be aware that if VType is an integer type, the high bits
  // of the result are silently discarded.
  VType CrossProd(const Self &vb) const;
  // Access component #b for read/write operations
  VType& operator[](const int b);
  // Access component #b for read only operations
  VType operator[](const int b) const;
  // Labeled Accessor methods.
  void x(const VType &v);
  VType x() const;
  void y(const VType &v);
  VType y() const;

  // return a pointer to the data array for interface with other libraries
  // like opencv
  VType* Data();
  const VType* Data() const;
  // Return the squared Euclidean norm of the vector.  Be aware that if VType
  // is an integer type, the high bits of the result are silently discarded.
  VType Norm2(void) const;
  // Return the Euclidean norm of the vector.  Note that if VType is an
  // integer type, the return value is correct only if the *squared* norm does
  // not overflow VType.
  FloatType Norm(void) const;
  // return the angle between "this" and v in radians
  FloatType Angle(const Self &v) const;
  // Return a normalized version of the vector if the norm of the
  // vector is not 0.  Not to be used with integer types.
  Self Normalize() const;
  // Compare two vectors, return true if all their components are equal
  // this operator is mostly useful for  integer types
  // for floating point types prefer "aequal"
  bool operator==(const Self &vb) const;
  bool operator!=(const Self &vb) const;
  // Compare two vectors, return true if all their components are within
  // a difference of margin.
  bool aequal(const Self &vb, FloatType margin) const;

  // Compare two vectors, these comparisons are mostly for interaction
  // with STL.
  bool operator<(const Self &vb) const;
  bool operator>(const Self &vb) const;
  bool operator<=(const Self &vb) const;
  bool operator>=(const Self &vb) const;

  // return a vector orthogonal to the current one
  // with the same norm and counterclockwise to it
  Self Ortho() const;
  // take the sqrt of each component and return a vector containing those values
  Self Sqrt() const;
  // Take the fabs of each component and return a vector containing
  // those values.
  Self Fabs() const;
  // Take the absolute value of each component and return a vector containing
  // those values.  This method should only be used when VType is a signed
  // integer type that is not wider than "int".
  Self Abs() const;
  // take the floor of each component and return a vector containing
  // those values
  Self Floor() const;
  // Take the ceil of each component and return a vector containing
  // those values.
  Self Ceil() const;
  // take the round of each component and return a vector containing those
  // values
  Self FRound() const;
  // take the round of each component and return an integer vector containing
  // those values
  Vector2<int> IRound() const;
  // Reset all the coordinates of the vector to 0
  void Clear();

  // return true if one of the components is not a number
  bool IsNaN() const;

  // return an invalid floating point vector
  static Self NaN();
};

// Multiply by a scalar.
template <typename ScalarType, typename VType>
Vector2<VType> operator*(const ScalarType k, const Vector2<VType> v);
// perform k /
template <typename ScalarType, typename VType>
Vector2<VType> operator/(const ScalarType k, const Vector2<VType> v);
// return a vector containing the max of v1 and v2 component by component
template <typename VType2>
Vector2<VType2> Max(const Vector2<VType2> &v1, const Vector2<VType2> &v2);
// return a vector containing the min of v1 and v2 component by component
template <typename VType2>
Vector2<VType2> Min(const Vector2<VType2> &v1, const Vector2<VType2> &v2);
// debug printing
template <typename VType2>
std::ostream &operator <<(std::ostream &out, // NOLINT
                          const Vector2<VType2> &va);

// TODO(user): Declare extern templates for these types.
typedef Vector2<uint8>  Vector2_b;
typedef Vector2<int>    Vector2_i;
typedef Vector2<float>  Vector2_f;
typedef Vector2<double> Vector2_d;

#endif  // UTIL_MATH_VECTOR2_H__
