#include <drivers/fs/root.hpp>

#include <drivers/fs/ext4/ext4.hpp>
#include <drivers/storage/root_device.hpp>
#include <drivers/storage/sector_reader.hpp>

namespace mikos::drivers::fs::root {
namespace {

using BlockDevice = storage::root_device::Device;
using Reader = storage::SectorReader<BlockDevice>;
using RootVolume = ext4::Volume<Reader>;

BlockDevice block_device;
Reader reader{block_device};
RootVolume volume;
bool ready{};

}  // namespace

bool initialize() {
  ready = false;
  if (!block_device.initialize() ||
      volume.initialize(reader) != Error::none) {
    return false;
  }
  const auto root = volume.root();
  if (!root || root.value.type != ext4::Type::directory) {
    return false;
  }
  ready = true;
  return true;
}

Result<Node> lookup(const char* path) {
  return ready ? volume.lookup_path(path)
               : Result<Node>::failure(Error::invalid_argument);
}

Error for_each(const Node& directory, void* context,
               DirectoryVisitor visitor) {
  if (!ready || visitor == nullptr) {
    return Error::invalid_argument;
  }
  return volume.for_each(directory, [&](const Entry& entry) {
    return visitor(context, entry);
  });
}

Result<u32> read(const Node& file, u64 offset, u8* output, u32 count) {
  return ready ? volume.read(file, offset, output, count)
               : Result<u32>::failure(Error::invalid_argument);
}

Result<u32> write(Node& file, u64 offset, const u8* input, u32 count) {
  return ready ? volume.write(file, offset, input, count)
               : Result<u32>::failure(Error::invalid_argument);
}

Error create(const char* path, const u8* input, u32 count) {
  return ready ? volume.create(path, input, count)
               : Error::invalid_argument;
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

Error sync() {
  return ready ? volume.sync() : Error::invalid_argument;
}

}  // namespace mikos::drivers::fs::root
