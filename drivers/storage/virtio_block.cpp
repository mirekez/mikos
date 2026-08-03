#include <drivers/net/virtio.hpp>
#include <drivers/storage/virtio_block.hpp>

namespace mikos::drivers::storage::virtio_block {
namespace {

using virtio::Feature;
using virtio::Status;

enum class Register : u32 {
  magic = 0x000,
  version = 0x004,
  device_id = 0x008,
  device_features = 0x010,
  device_features_select = 0x014,
  driver_features = 0x020,
  driver_features_select = 0x024,
  queue_select = 0x030,
  queue_size_max = 0x034,
  queue_size = 0x038,
  queue_ready = 0x044,
  queue_notify = 0x050,
  status = 0x070,
  descriptor_low = 0x080,
  available_low = 0x090,
  used_low = 0x0a0,
};

class Mmio {
 public:
  constexpr Mmio() = default;
  explicit constexpr Mmio(volatile u32* base) : base_{base} {}

  [[nodiscard]] constexpr explicit operator bool() const {
    return base_ != nullptr;
  }

  [[nodiscard]] u32 read(Register reg) const {
    return base_[static_cast<u32>(reg) / sizeof(u32)];
  }

  void write(Register reg, u32 value) const {
    base_[static_cast<u32>(reg) / sizeof(u32)] = value;
  }

  void set_address(Register low, usize address) const {
    write(low, static_cast<u32>(address));
    base_[(static_cast<u32>(low) + 4) / sizeof(u32)] = 0;
  }

  [[nodiscard]] u32 config32(u32 offset) const {
    return base_[(0x100 + offset) / sizeof(u32)];
  }

  [[nodiscard]] virtio::FeatureSet device_features() const {
    write(Register::device_features_select, 0);
    const u32 low = read(Register::device_features);
    write(Register::device_features_select, 1);
    const u32 high = read(Register::device_features);
    return virtio::FeatureSet::from_banks(low, high);
  }

  void accept_features(virtio::FeatureSet features) const {
    write(Register::driver_features_select, 0);
    write(Register::driver_features, features.low());
    write(Register::driver_features_select, 1);
    write(Register::driver_features, features.high());
  }

  [[nodiscard]] static Mmio find_block_device() {
    for (u32 slot = 8; slot != 0; --slot) {
      Mmio candidate{reinterpret_cast<volatile u32*>(
          0x10000000 + slot * 0x1000)};
      if (candidate.read(Register::magic) == 0x74726976 &&
          candidate.read(Register::version) == 2 &&
          candidate.read(Register::device_id) == 2) {
        return candidate;
      }
    }
    return {};
  }

 private:
  volatile u32* base_{};
};

struct [[gnu::packed]] Request {
  u32 type;
  u32 reserved;
  u64 sector;
};

class Device {
 public:
  [[nodiscard]] bool initialize() {
    mmio_ = Mmio::find_block_device();
    if (!mmio_) {
      return false;
    }
    mmio_.write(Register::status, 0);
    set_status(Status::acknowledge);
    set_status(Status::driver);
    const auto offered = mmio_.device_features();
    if (!offered.contains(Feature::version_1)) {
      fail();
      return false;
    }
    const auto accepted =
        offered & virtio::FeatureSet{Feature::version_1,
                                     Feature::block_flush};
    mmio_.accept_features(accepted);
    set_status(Status::features_ok);
    if ((mmio_.read(Register::status) &
         virtio::bits(Status::features_ok)) == 0) {
      fail();
      return false;
    }

    queue_.reset();
    mmio_.write(Register::queue_select, 0);
    if (mmio_.read(Register::queue_size_max) < queue_.size) {
      fail();
      return false;
    }
    mmio_.write(Register::queue_size, queue_.size);
    mmio_.set_address(Register::descriptor_low,
                      queue_.descriptor_address());
    mmio_.set_address(Register::available_low,
                      queue_.available_address());
    mmio_.set_address(Register::used_low, queue_.used_address());
    mmio_.write(Register::queue_ready, 1);
    if (mmio_.read(Register::queue_ready) != 1) {
      fail();
      return false;
    }
    sectors_ = static_cast<u64>(mmio_.config32(0)) |
               (static_cast<u64>(mmio_.config32(4)) << 32);
    flush_supported_ = offered.contains(Feature::block_flush);
    set_status(Status::ready);
    return sectors_ != 0;
  }

  [[nodiscard]] u64 sector_count() const { return sectors_; }

  [[nodiscard]] bool read_sectors(u64 sector, u8* output,
                                  u32 byte_count) {
    return transfer(0, sector, output, byte_count,
                    virtio::Access::device_writes);
  }

  [[nodiscard]] bool write_sectors(u64 sector, const u8* input,
                                   u32 byte_count) {
    return transfer(1, sector, input, byte_count,
                    virtio::Access::device_reads);
  }

  [[nodiscard]] bool flush() {
    if (!flush_supported_) {
      return false;
    }
    request_ = Request{4, 0, 0};
    status_ = 0xff;
    queue_.describe(0, &request_, sizeof(request_),
                    virtio::Access::device_reads, 1);
    queue_.describe(1, &status_, sizeof(status_),
                    virtio::Access::device_writes);
    return submit();
  }

 private:
  [[nodiscard]] bool transfer(u32 type, u64 sector, const void* data,
                              u32 byte_count, virtio::Access access) {
    if (data == nullptr || byte_count == 0 ||
        byte_count % sector_size != 0 || sector >= sectors_ ||
        byte_count / sector_size > sectors_ - sector) {
      return false;
    }
    request_ = Request{type, 0, sector};
    status_ = 0xff;
    queue_.describe(0, &request_, sizeof(request_),
                    virtio::Access::device_reads, 1);
    queue_.describe(1, data, byte_count, access, 2);
    queue_.describe(2, &status_, sizeof(status_),
                    virtio::Access::device_writes);
    return submit();
  }

  [[nodiscard]] bool submit() {
    queue_.offer(0, dma_fence);
    mmio_.write(Register::queue_notify, 0);
    for (u32 spin = 0; spin < 10'000'000; ++spin) {
      const auto completion = queue_.take(dma_fence);
      if (completion.ready) {
        return completion.element.id == 0 && status_ == 0;
      }
    }
    fail();
    return false;
  }

  static void dma_fence() {
    asm volatile("fence rw, rw" ::: "memory");
  }

  void set_status(Status status) {
    mmio_.write(Register::status,
                mmio_.read(Register::status) | virtio::bits(status));
  }

  void fail() { set_status(Status::failed); }

  Mmio mmio_{};
  virtio::SplitQueue<8> queue_{};
  alignas(8) Request request_{};
  u8 status_{};
  u64 sectors_{};
  bool flush_supported_{};
};

Device device;

}  // namespace

bool initialize() { return device.initialize(); }
u64 sector_count() { return device.sector_count(); }
bool read_sector(u64 sector, u8* output) {
  return device.read_sectors(sector, output, sector_size);
}
bool read_sectors(u64 sector, u8* output, u32 byte_count) {
  return device.read_sectors(sector, output, byte_count);
}
bool write_sector(u64 sector, const u8* input) {
  return device.write_sectors(sector, input, sector_size);
}
bool write_sectors(u64 sector, const u8* input, u32 byte_count) {
  return device.write_sectors(sector, input, byte_count);
}
bool flush() { return device.flush(); }

}  // namespace mikos::drivers::storage::virtio_block
