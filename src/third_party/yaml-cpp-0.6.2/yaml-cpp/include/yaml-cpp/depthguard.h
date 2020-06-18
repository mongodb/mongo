#ifndef DEPTH_GUARD_H_00000000000000000000000000000000000000000000000000000000
#define DEPTH_GUARD_H_00000000000000000000000000000000000000000000000000000000

#if defined(_MSC_VER) ||                                            \
    (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || \
     (__GNUC__ >= 4))  // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include "exceptions.h"

namespace YAML {

/**
 * @brief The DeepRecursion class
 *  An exception class which is thrown by DepthGuard. Ideally it should be
 * a member of DepthGuard. However, DepthGuard is a templated class which means
 * that any catch points would then need to know the template parameters. It is
 * simpler for clients to not have to know at the catch point what was the
 * maximum depth.
 */
class DeepRecursion : public ParserException {
public:
  virtual ~DeepRecursion() = default;

  DeepRecursion(int depth, const Mark& mark_, const std::string& msg_);

  // Returns the recursion depth when the exception was thrown
  int depth() const {
    return m_depth;
  }

private:
  int m_depth = 0;
};

/**
 * @brief The DepthGuard class
 *  DepthGuard takes a reference to an integer. It increments the integer upon
 * construction of DepthGuard and decrements the integer upon destruction.
 *
 * If the integer would be incremented past max_depth, then an exception is
 * thrown. This is ideally geared toward guarding against deep recursion.
 *
 * @param max_depth
 *  compile-time configurable maximum depth.
 */
template <int max_depth = 2000>
class DepthGuard final {
public:
  DepthGuard(int & depth_, const Mark& mark_, const std::string& msg_) : m_depth(depth_) {
    ++m_depth;
    if ( max_depth <= m_depth ) {
        throw DeepRecursion{m_depth, mark_, msg_};
    }
  }

  DepthGuard(const DepthGuard & copy_ctor) = delete;
  DepthGuard(DepthGuard && move_ctor) = delete;
  DepthGuard & operator=(const DepthGuard & copy_assign) = delete;
  DepthGuard & operator=(DepthGuard && move_assign) = delete;

  ~DepthGuard() {
    --m_depth;
  }

  int current_depth() const {
    return m_depth;
  }

private:
    int & m_depth;
};

} // namespace YAML

#endif // DEPTH_GUARD_H_00000000000000000000000000000000000000000000000000000000
