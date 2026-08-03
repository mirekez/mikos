#include <drivers/fs/ext4/ext4.hpp>

namespace {

using namespace mikos;
using namespace mikos::drivers::fs::ext4;

struct Device {
  [[nodiscard]] u64 size() const { return 0; }
  [[nodiscard]] bool read(u64, u8*, u32) { return false; }
};

[[maybe_unused]] void instantiate(Device& device, const Node& node,
                                  u8* output) {
  auto mounted = Volume<Device>::mount(device);
  if (!mounted) {
    return;
  }
  static_cast<void>(mounted.value.lookup_path("/file"));
  static_cast<void>(mounted.value.read(node, 0, output, 1));
  static_cast<void>(mounted.value.for_each(
      node, [](const Entry&) { return true; }));
}

}  // namespace
