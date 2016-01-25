#ifndef ANCHORDICT_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define ANCHORDICT_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) ||                                            \
    (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || \
     (__GNUC__ >= 4))  // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include <vector>

#include "../anchor.h"

namespace YAML {
/// AnchorDict
/// . An object that stores and retrieves values correlating to anchor_t
///   values.
/// . Efficient implementation that can make assumptions about how anchor_t
///   values are assigned by the Parser class.
template <class T>
class AnchorDict {
 public:
  void Register(anchor_t anchor, T value) {
    if (anchor > m_data.size()) {
      m_data.resize(anchor);
    }
    m_data[anchor - 1] = value;
  }

  T Get(anchor_t anchor) const { return m_data[anchor - 1]; }

 private:
  std::vector<T> m_data;
};
}

#endif  // ANCHORDICT_H_62B23520_7C8E_11DE_8A39_0800200C9A66
