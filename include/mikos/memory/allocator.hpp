#pragma once

#include <mikos/memory/arena.hpp>
#include <limits>
#include <type_traits>

namespace mikos::memory {

// A checked allocation can be handed to the next standard-container allocate
// call. The ticket releases unused memory automatically. One ticket belongs to
// one operation/resource; it must not be shared across tasks or recursive work.
class Resource {
 public:
  explicit Resource(Arena& arena) : arena_(arena) {}
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;

  class Reservation {
   public:
    Reservation(Resource& resource, usize bytes, usize alignment)
        : resource_(resource) {
      if (resource_.reserved_ != nullptr) allocation_contract_failure();
      pointer_ = resource_.arena_.try_allocate(bytes, alignment);
      if (pointer_ != nullptr) {
        resource_.reserved_ = pointer_;
        resource_.reserved_bytes_ = bytes;
        resource_.reserved_alignment_ = alignment;
      }
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() {
      if (pointer_ != nullptr && resource_.reserved_ == pointer_) {
        resource_.reserved_ = nullptr;
        if (!resource_.arena_.release(pointer_)) allocation_contract_failure();
      }
    }
    explicit operator bool() const { return pointer_ != nullptr; }
   private:
    Resource& resource_;
    void* pointer_{};
  };

  [[nodiscard]] void* allocate(usize bytes, usize alignment) {
    if (reserved_ != nullptr) {
      if (bytes > reserved_bytes_ || alignment > reserved_alignment_)
        allocation_contract_failure();
      void* result = reserved_;
      reserved_ = nullptr;
      return result;
    }
    auto* result = arena_.try_allocate(bytes, alignment);
    if (result == nullptr) allocation_failure();
    return result;
  }
  void deallocate(void* pointer) {
    if (!arena_.release(pointer)) allocation_contract_failure();
  }
 private:
  Arena& arena_;
  void* reserved_{};
  usize reserved_bytes_{};
  usize reserved_alignment_{};
};

template <typename T>
class KernelAllocator {
 public:
  using value_type = T;
  using is_always_equal = std::false_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  explicit KernelAllocator(Resource& resource) noexcept : resource_(&resource) {}
  template <typename U>
  KernelAllocator(const KernelAllocator<U>& other) noexcept
      : resource_(&other.resource()) {}

  [[nodiscard]] T* allocate(usize n) {
    if (n > max_size()) allocation_failure();
    return static_cast<T*>(resource_->allocate(n == 0 ? sizeof(T) : n * sizeof(T),
                                               alignof(T)));
  }
  void deallocate(T* pointer, usize) noexcept { resource_->deallocate(pointer); }
  [[nodiscard]] constexpr usize max_size() const noexcept {
    return std::numeric_limits<usize>::max() / sizeof(T);
  }
  [[nodiscard]] Resource& resource() const noexcept { return *resource_; }
  template <typename U>
  bool operator==(const KernelAllocator<U>& other) const noexcept {
    return resource_ == &other.resource();
  }
 private:
  Resource* resource_;
};
}  // namespace mikos::memory
