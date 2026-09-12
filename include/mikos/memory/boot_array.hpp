#pragma once

#include <mikos/base.hpp>
#include <new>

namespace mikos::memory {
// Keep mostly-empty tables with nonzero default members out of the file image.
// Explicit boot construction establishes all lifetimes and defaults. RAM usage
// is unchanged; never access before initialize(). No global constructors.
template <typename T, usize N>
class BootArray {
 public:
  // Default-initialize each class element, including nested arrays. Do not use
  // aggregate array-new {} here: Clang 21 does not apply nested default member
  // initializers consistently in that form (covered by the host regression).
  void initialize() { ::new (storage_) T[N]; }
  T& operator[](usize n) { return begin()[n]; }
  T* begin() { return __builtin_launder(reinterpret_cast<T*>(storage_)); }
  T* end() { return begin() + N; }
 private:
  alignas(T) unsigned char storage_[sizeof(T) * N]{};
};
}  // namespace mikos::memory
