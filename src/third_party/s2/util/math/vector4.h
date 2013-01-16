// Copyright 2003 Google, Inc.
// All Rights Reserved.
//
//
// A simple class to handle vectors in 4D
// See the vector4-inl.h file for more details


#ifndef UTIL_MATH_VECTOR4_H__
#define UTIL_MATH_VECTOR4_H__

#include <iostream>
using std::ostream;
using std::cout;
using std::endl;
  // NOLINT(readability/streams)
#include "base/basictypes.h"

template <typename VType> class Vector4;
// TODO(user): Look into creating conversion operators to remove the
// need to forward-declare Vector2 and Vector3.
template <typename VType> class Vector2;
template <typename VType> class Vector3;

// Template class for 4D vectors
template <typename VType>
class Vector4 {
 private:
  VType c_[4];

  // FloatType is the type returned by Norm().  This method is special because
  // it returns floating-point values even when VType is an integer.
  typedef typename base::if_<base::is_integral<VType>::value,
                             double, VType>::type FloatType;

 public:
  typedef Vector4<VType> Self;
  typedef VType BaseType;
  // Create a new vector (0,0)
  Vector4();
  // Create a new vector (x,y,z,w)
  Vector4(const VType x, const VType y, const VType z, const VType w);
  // Create a new copy of the vector vb
  Vector4(const Self &vb);
  // Create a new 4D vector from 2D vector and two scalars
  // (vb.x,vb.y,z,w)
  Vector4(const Vector2<VType> &vb, const VType z, const VType w);
  // Create a 4D vector from two 2D vectors (vb1.x,vb1.y,vb2.x,vb2.y)
  Vector4(const Vector2<VType> &vb1, const Vector2<VType> &vb2);
  // Create a new 4D vector from 3D vector and scalar
  // (vb.x,vb.y,vb.z,w)
  Vector4(const Vector3<VType> &vb, const VType w);
  // Convert from another vector type
  template <typename VType2>
  static Self Cast(const Vector4<VType2> &vb);
  // Compare two vectors, return true if all their components are equal
  bool operator==(const Self& vb) const;
  bool operator!=(const Self& vb) const;
  // Compare two vectors, return true if all their components are within
  // a difference of margin.
  bool aequal(const Self &vb, FloatType margin) const;
  // Compare two vectors, these comparisons are mostly for interaction
  // with STL.
  bool operator<(const Self &vb) const;
  bool operator>(const Self &vb) const;
  bool operator<=(const Self &vb) const;
  bool operator>=(const Self &vb) const;

  // Return the size of the vector
  static int Size() { return 4; }
  // Modify the coordinates of the current vector
  void Set(const VType x, const VType y, const VType z, const VType w);
  Self& operator=(const Self& vb);
  // add two vectors, component by component
  Self& operator+=(const Self& vb);
  // subtract two vectors, component by component
  Self& operator-=(const Self& vb);
  // multiply a vector by a scalar
  Self& operator*=(const VType k);
  // divide a vector by a scalar : implemented that way for integer vectors
  Self& operator/=(const VType k);
  // multiply two vectors component by component
  Self MulComponents(const Self &vb) const;
  // divide two vectors component by component
  Self DivComponents(const Self &vb) const;
  // add two vectors, component by component
  Self operator+(const Self &vb) const;
  // subtract two vectors, component by component
  Self operator-(const Self &vb) const;
  // Dot product.  Be aware that if VType is an integer type, the high bits of
  // the result are silently discarded.
  VType DotProd(const Self &vb) const;
  // Multiplication by a scalar
  Self operator*(const VType k) const;
  // Division by a scalar
  Self operator/(const VType k) const;
  // Access component #b for read/write operations
  VType& operator[](const int b);
  // Access component #b for read only operations
  VType operator[](const int b) const;
  // Labeled Accessor methods.
  void x(const VType &v);
  VType x() const;
  void y(const VType &v);
  VType y() const;
  void z(const VType &v);
  VType z() const;
  void w(const VType &v);
  VType w() const;
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
  // Return a normalized version of the vector if the norm of the
  // vector is not 0.  Not to be used with integer types.
  Self Normalize() const;
  // take the sqrt of each component and return a vector containing those values
  Self Sqrt() const;
  // take the fabs of each component and return a vector containing those values
  Self Fabs() const;
  // Take the absolute value of each component and return a vector containing
  // those values.  This method should only be used when VType is a signed
  // integer type that is not wider than "int".
  Self Abs() const;
  // take the floor of each component and return a vector containing
  // those values
  Self Floor() const;
  // take the ceil of each component and return a vector containing those values
  Self Ceil() const;
  // take the round of each component and return a vector containing those
  // values
  Self FRound() const;
  // take the round of each component and return an integer vector containing
  // those values
  Vector4<int> IRound() const;
  // Reset all the coordinates of the vector to 0
  void Clear();

  // return true if one of the components is not a number
  bool IsNaN() const;

  // return an invalid floating point vector
  static Self NaN();
};

// change the sign of the components of a vector
template <typename VType>
Vector4<VType> operator-(const Vector4<VType> &vb);
// multiply by a scalar
template <typename ScalarType, typename VType>
Vector4<VType> operator*(const ScalarType k, const Vector4<VType> &v);
// perform k /
template <typename ScalarType, typename VType>
Vector4<VType> operator/(const ScalarType k, const Vector4<VType> &v);
// return a vector containing the max of v1 and v2 component by component
template <typename VType>
Vector4<VType> Max(const Vector4<VType> &v1, const Vector4<VType> &v2);
// return a vector containing the min of v1 and v2 component by component
template <typename VType>
Vector4<VType> Min(const Vector4<VType> &v1, const Vector4<VType> &v2);
// debug printing
template <typename VType>
std::ostream &operator <<(std::ostream &out,  // NOLINT
                          const Vector4<VType> &va);

typedef Vector4<uint8>  Vector4_b;
typedef Vector4<int>    Vector4_i;
typedef Vector4<float>  Vector4_f;
typedef Vector4<double> Vector4_d;

#endif  // UTIL_MATH_VECTOR4_H__
