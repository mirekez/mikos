#include <drivers/fs/ext4/ext4.hpp>
#include <drivers/fs/root.hpp>
#include <drivers/storage/root_device.hpp>
#include <drivers/storage/sector_reader.hpp>

namespace mikos::drivers::fs::root {
namespace {

using BlockDevice = storage::root_device::Device;
using Reader = storage::SectorReader<BlockDevice>;
using RootVolume = ext4::Volume<Reader>;

// One non-copyable owner keeps the device, its cache, mounted volume and
// readiness state together. Internal reader pointers cannot escape by copying.
class RootFilesystem {
 public:
  constexpr RootFilesystem() = default;
  RootFilesystem(const RootFilesystem&) = delete;
  RootFilesystem& operator=(const RootFilesystem&) = delete;

  bool initialize() {
    ready = false;
    if (!block_device.initialize() ||
        volume.initialize(reader) != Error::none) {
      return false;
    }
    const auto root = volume.root();
    if (!root || root->type != ext4::Type::directory) {
      return false;
    }
    ready = true;
    return true;
  }

  Result<Node> lookup(const char* path) {
    return ready ? volume.lookup_path(path)
                 : std::unexpected(Error::invalid_argument);
  }

  Error for_each(const Node& directory, void* context,
                 DirectoryVisitor visitor) {
    if (!ready || visitor == nullptr) {
      return Error::invalid_argument;
    }
    return volume.for_each(
        directory, [&](const Entry& entry) { return visitor(context, entry); });
  }

  Result<u32> read(const Node& file, u64 offset, u8* output, u32 count) {
    return ready ? volume.read(file, offset, output, count)
                 : std::unexpected(Error::invalid_argument);
  }

  Result<u32> write(Node& file, u64 offset, const u8* input, u32 count) {
    return ready ? volume.write(file, offset, input, count)
                 : std::unexpected(Error::invalid_argument);
  }

  Error create(const char* path, const u8* input, u32 count) {
    return ready ? volume.create(path, input, count) : Error::invalid_argument;
  }

  Error mkdir(const char* path, u16 mode) {
    return ready ? volume.mkdir(path, mode) : Error::invalid_argument;
  }

  Error truncate(Node& file, u64 size) {
    return ready ? volume.truncate(file, size) : Error::invalid_argument;
  }

  Error remove(const char* path) {
    return ready ? volume.remove(path) : Error::invalid_argument;
  }

  Error move(const char* source, const char* destination) {
    return ready ? volume.move(source, destination) : Error::invalid_argument;
  }

  Error sync() { return ready ? volume.sync() : Error::invalid_argument; }

 private:
  BlockDevice block_device;
  Reader reader{block_device};
  RootVolume volume;
  bool ready{};
};

constinit RootFilesystem filesystem;
}  // namespace

bool initialize() { return filesystem.initialize(); }
Result<Node> lookup(const char* path) { return filesystem.lookup(path); }
Error for_each(const Node& directory, void* context, DirectoryVisitor visitor) {
  return filesystem.for_each(directory, context, visitor);
}
Result<u32> read(const Node& file, u64 offset, u8* output, u32 count) {
  return filesystem.read(file, offset, output, count);
}
Result<u32> write(Node& file, u64 offset, const u8* input, u32 count) {
  return filesystem.write(file, offset, input, count);
}
Error create(const char* path, const u8* input, u32 count) {
  return filesystem.create(path, input, count);
}
Error mkdir(const char* path, u16 mode) { return filesystem.mkdir(path, mode); }
Error truncate(Node& file, u64 size) { return filesystem.truncate(file, size); }
Error remove(const char* path) { return filesystem.remove(path); }
Error move(const char* from, const char* to) {
  return filesystem.move(from, to);
}
Error sync() { return filesystem.sync(); }

}  // namespace mikos::drivers::fs::root
