// Copyright 2003 Google, Inc.
// All Rights Reserved.
//
//
// A simple class to handle vectors in 2D
// The aim of this class is to be able to manipulate vectors in 2D
// as naturally as possible and make calculations readable.
// For that reason, the operators +, -, * are overloaded.
// (Reading a = a + b*2 - c is much easier to read than
// a = Sub(Add(a, Mul(b,2)),c)   )
// The code generated using this vector class is easily optimized by
// the compiler and does not generate overhead compared to manually
// writing the operations component by component
// (e.g a.x = b.x + c.x; a.y = b.y + c.y...)
//
// Operator overload is not usually allowed, but in this case an
// exemption has been granted by the C++ style committee.
//
// Please be careful about overflows when using those vectors with integer types
// The calculations are carried with the same type as the vector's components
// type. eg : if you are using uint8 as the base type, all values will be modulo
// 256.
// This feature is necessary to use the class in a more general framework with
// VType != plain old data type.

#ifndef UTIL_MATH_VECTOR2_INL_H__
#define UTIL_MATH_VECTOR2_INL_H__

#include "util/math/vector2.h"

#include <math.h>
#include "base/basictypes.h"
#include "base/logging.h"
#include "base/template_util.h"
#include "base/type_traits.h"
#include "util/math/mathutil.h"
#include "util/math/vector3.h"
#include "util/math/vector4.h"

template <typename VType>
Vector2<VType>::Vector2() {
  Clear();
}
template <typename VType>
Vector2<VType>::Vector2(const VType x, const VType y) {
  c_[0] = x;
  c_[1] = y;
}
template <typename VType>
Vector2<VType>::Vector2(const Self &vb) {
  c_[0] = vb.c_[0];
  c_[1] = vb.c_[1];
}
template <typename VType>
Vector2<VType>::Vector2(const Vector3<VType> &vb) {
  c_[0] = vb.x();
  c_[1] = vb.y();
}
template <typename VType>
Vector2<VType>::Vector2(const Vector4<VType> &vb) {
  c_[0] = vb.x();
  c_[1] = vb.y();
}

template <typename VType> template <typename VType2>
Vector2<VType> Vector2<VType>::Cast(const Vector2<VType2> &vb) {
  return Self(static_cast<VType>(vb[0]),
              static_cast<VType>(vb[1]));
}

template <typename VType>
void Vector2<VType>::Set(const VType x, const VType y) {
  c_[0] = x;
  c_[1] = y;
}

template <typename VType>
const Vector2<VType>& Vector2<VType>::operator=(const Self &vb) {
  c_[0] = vb.c_[0];
  c_[1] = vb.c_[1];
  return (*this);
}

template <typename VType>
Vector2<VType>& Vector2<VType>::operator+=(const Self &vb) {
  c_[0] += vb.c_[0];
  c_[1] += vb.c_[1];
  return (*this);
}

template <typename VType>
Vector2<VType>& Vector2<VType>::operator-=(const Self &vb) {
  c_[0] -= vb.c_[0];
  c_[1] -= vb.c_[1];
  return (*this);
}

template <typename VType>
Vector2<VType>& Vector2<VType>::operator*=(const VType k) {
  c_[0] *= k;
  c_[1] *= k;
  return (*this);
}

template <typename VType>
Vector2<VType>& Vector2<VType>::operator/=(const VType k) {
  c_[0] /= k;
  c_[1] /= k;
  return (*this);
}

template <typename VType>
Vector2<VType> Vector2<VType>::MulComponents(const Self &vb) const {
  return Self(c_[0] * vb.c_[0], c_[1] * vb.c_[1]);
}

template <typename VType>
Vector2<VType> Vector2<VType>::DivComponents(const Self &vb) const {
  return Self(c_[0] / vb.c_[0], c_[1] / vb.c_[1]);
}

template <typename VType>
Vector2<VType> Vector2<VType>::operator+(const Self &vb) const {
  return Self(*this) += vb;
}

template <typename VType>
Vector2<VType> Vector2<VType>::operator-(const Self &vb) const {
  return Self(*this) -= vb;
}

template <typename VType>
Vector2<VType> Vector2<VType>::operator-() const {
  return Self(-c_[0], -c_[1]);
}

template <typename VType>
VType Vector2<VType>::DotProd(const Self &vb) const {
  return c_[0] * vb.c_[0] + c_[1] * vb.c_[1];
}

template <typename VType>
Vector2<VType> Vector2<VType>::operator*(const VType k) const {
  return Self(*this) *= k;
}

template <typename VType>
Vector2<VType> Vector2<VType>::operator/(const VType k) const {
  return Self(*this) /= k;
}

template <typename VType>
VType Vector2<VType>::CrossProd(const Self &vb) const {
  return c_[0] * vb.c_[1] - c_[1] * vb.c_[0];
}

template <typename VType>
VType& Vector2<VType>::operator[](const int b) {
  DCHECK(b >= 0);
  DCHECK(b <= 1);
  return c_[b];
}

template <typename VType>
VType Vector2<VType>::operator[](const int b) const {
  DCHECK(b >= 0);
  DCHECK(b <= 1);
  return c_[b];
}

template <typename VType>
void Vector2<VType>::x(const VType &v) {
  c_[0] = v;
}

template <typename VType>
VType Vector2<VType>::x() const {
  return c_[0];
}

template <typename VType>
void Vector2<VType>::y(const VType &v) {
  c_[1] = v;
}

template <typename VType>
VType Vector2<VType>::y() const {
  return c_[1];
}



template <typename VType>
VType* Vector2<VType>::Data() {
  return reinterpret_cast<VType*>(c_);
}

template <typename VType>
const VType* Vector2<VType>::Data() const {
  return reinterpret_cast<const VType*>(c_);
}


template <typename VType>
VType Vector2<VType>::Norm2(void) const {
  return c_[0]*c_[0] + c_[1]*c_[1];
}


template <typename VType>
typename Vector2<VType>::FloatType Vector2<VType>::Norm(void) const {
  return sqrt(Norm2());
}

template <typename VType>
typename Vector2<VType>::FloatType Vector2<VType>::Angle(const Self &v) const {
  return atan2(this->CrossProd(v), this->DotProd(v));
}

template <typename VType>
Vector2<VType> Vector2<VType>::Normalize() const {
  COMPILE_ASSERT(!base::is_integral<VType>::value, must_be_floating_point);
  VType n = Norm();
  if (n != 0) {
    n = 1.0 / n;
  }
  return Self(*this) *= n;
}

template <typename VType>
bool Vector2<VType>::operator==(const Self &vb) const {
  return  (c_[0] == vb.c_[0]) && (c_[1] == vb.c_[1]);
}

template <typename VType>
bool Vector2<VType>::operator!=(const Self &vb) const {
  return  (c_[0] != vb.c_[0]) || (c_[1] != vb.c_[1]);
}

template <typename VType>
bool Vector2<VType>::aequal(const Self &vb, FloatType margin) const {
  return (fabs(c_[0]-vb.c_[0]) < margin) && (fabs(c_[1]-vb.c_[1]) < margin);
}

template <typename VType>
bool Vector2<VType>::operator<(const Self &vb) const {
  if ( c_[0] < vb.c_[0] ) return true;
  if ( vb.c_[0] < c_[0] ) return false;
  if ( c_[1] < vb.c_[1] ) return true;
  return false;
}

template <typename VType>
bool Vector2<VType>::operator>(const Self &vb) const {
  return vb.operator<(*this);
}

template <typename VType>
bool Vector2<VType>::operator<=(const Self &vb) const {
  return !operator>(vb);
}

template <typename VType>
bool Vector2<VType>::operator>=(const Self &vb) const {
  return !operator<(vb);
}

template <typename VType>
Vector2<VType> Vector2<VType>::Ortho() const {
  return Self(-c_[1], c_[0]);
}

template <typename VType>
Vector2<VType> Vector2<VType>::Sqrt() const {
  return Self(sqrt(c_[0]), sqrt(c_[1]));
}

template <typename VType>
Vector2<VType> Vector2<VType>::Fabs() const {
  return Self(fabs(c_[0]), fabs(c_[1]));
}

template <typename VType>
Vector2<VType> Vector2<VType>::Abs() const {
  COMPILE_ASSERT(base::is_integral<VType>::value, use_Fabs_for_float_types);
  COMPILE_ASSERT(static_cast<VType>(-1) == -1, type_must_be_signed);
  COMPILE_ASSERT(sizeof(VType) <= sizeof(int), Abs_truncates_to_int);
  return Self(abs(c_[0]), abs(c_[1]));
}

template <typename VType>
Vector2<VType> Vector2<VType>::Floor() const {
  return Self(floor(c_[0]), floor(c_[1]));
}


template <typename VType>
Vector2<VType> Vector2<VType>::Ceil() const {
  return Self(ceil(c_[0]), ceil(c_[1]));
}


template <typename VType>
Vector2<VType> Vector2<VType>::FRound() const {
  return Self(rint(c_[0]), rint(c_[1]));
}

template <typename VType>
Vector2<int> Vector2<VType>::IRound() const {
  return Vector2<int>(lrint(c_[0]), lrint(c_[1]));
}

template <typename VType>
void Vector2<VType>::Clear() {
  c_[1] = c_[0] = VType();
}

template <typename VType>
bool Vector2<VType>::IsNaN() const {
  return isnan(c_[0]) || isnan(c_[1]);
}

template <typename VType>
Vector2<VType> Vector2<VType>::NaN() {
  return Self(MathUtil::NaN(), MathUtil::NaN());
}

template <typename ScalarType, typename VType2>
Vector2<VType2> operator*(const ScalarType k, const Vector2<VType2> v) {
  return Vector2<VType2>( k * v[0], k * v[1]);
}

template <typename ScalarType, typename VType2>
Vector2<VType2> operator/(const ScalarType k, const Vector2<VType2> v) {
  return Vector2<VType2>(k / v[0], k / v[1]);
}

template <typename VType>
Vector2<VType> Max(const Vector2<VType> &v1, const Vector2<VType> &v2) {
  return Vector2<VType>(max(v1[0], v2[0]), max(v1[1], v2[1]));
}

template <typename VType>
Vector2<VType> Min(const Vector2<VType> &v1, const Vector2<VType> &v2) {
  return Vector2<VType>(min(v1[0], v2[0]), min(v1[1], v2[1]));
}

template <typename VType>
std::ostream &operator <<(std::ostream &out, const Vector2<VType> &va) {
  out << "["
      << va[0] << ", "
      << va[1] << "]";
  return out;
}

// TODO(user): Vector2<T> does not actually satisfy the definition of a POD
// type even when T is a POD. Pretending that Vector2<T> is a POD probably
// won't cause any immediate problems, but eventually this should be fixed.
PROPAGATE_POD_FROM_TEMPLATE_ARGUMENT(Vector2);

#endif  // UTIL_MATH_VECTOR2_INL_H__
