#pragma once

#include <mikos/base.hpp>
#if __has_include(<inplace_vector>)
#include <inplace_vector>
#endif
#if !defined(__cpp_lib_inplace_vector)
#include <beman/inplace_vector/inplace_vector.hpp>
#endif

namespace mikos {
#if defined(__cpp_lib_inplace_vector)
template <typename T, usize N> using inplace_vector = std::inplace_vector<T, N>;
#else
template <typename T, usize N>
using inplace_vector = beman::inplace_vector::inplace_vector<T, N>;
#endif
}
