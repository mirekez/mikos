#include <mikos/drivers/virtio_net.hpp>

namespace mikos::drivers::net {
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

  [[nodiscard]] u8 config(u32 index) const {
    return *(reinterpret_cast<volatile u8*>(base_) + 0x100 + index);
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

  [[nodiscard]] static Mmio find_network_device() {
    for (u32 slot = 8; slot != 0; --slot) {
      Mmio candidate{reinterpret_cast<volatile u32*>(0x10000000 +
                                                     slot * 0x1000)};
      if (candidate.read(Register::magic) == 0x74726976 &&
          candidate.read(Register::version) == 2 &&
          candidate.read(Register::device_id) == 1) {
        return candidate;
      }
    }
    return {};
  }

 private:
  volatile u32* base_{};
};

class MmioTransport {
 public:
  using Queue = virtio::SplitQueue<8>;

  [[nodiscard]] bool begin() {
    mmio_ = Mmio::find_network_device();
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
    accepted_features_ =
        offered & virtio::FeatureSet{Feature::mac, Feature::version_1};
    mmio_.accept_features(accepted_features_);
    set_status(Status::features_ok);
    return (mmio_.read(Register::status) &
            virtio::bits(Status::features_ok)) != 0;
  }

  [[nodiscard]] bool configure_queue(u16 number, const Queue& queue) {
    mmio_.write(Register::queue_select, number);
    if (mmio_.read(Register::queue_size_max) < Queue::size) {
      return false;
    }
    mmio_.write(Register::queue_size, Queue::size);
    mmio_.set_address(Register::descriptor_low, queue.descriptor_address());
    mmio_.set_address(Register::available_low, queue.available_address());
    mmio_.set_address(Register::used_low, queue.used_address());
    mmio_.write(Register::queue_ready, 1);
    return mmio_.read(Register::queue_ready) == 1;
  }

  [[nodiscard]] bool finish(MacAddress& mac) {
    if (accepted_features_.contains(Feature::mac)) {
      for (u32 i = 0; i < 6; ++i) {
        mac.octet[i] = mmio_.config(i);
      }
    } else {
      mac = MacAddress{{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
    }
    set_status(Status::ready);
    return true;
  }

  void fail() { set_status(Status::failed); }
  void notify(u16 queue) { mmio_.write(Register::queue_notify, queue); }

  static void dma_fence() {
    asm volatile("fence rw, rw" ::: "memory");
  }

 private:
  void set_status(Status status) {
    mmio_.write(Register::status,
                mmio_.read(Register::status) | virtio::bits(status));
  }

  Mmio mmio_{};
  virtio::FeatureSet accepted_features_{};
};

using Device =
    virtio::BasicNetDevice<MmioTransport, MmioTransport::Queue, 12, 8>;
Device device;

}  // namespace

bool initialize() { return device.initialize(); }
MacAddress mac_address() { return device.mac_address(); }
bool receive(Frame& frame) { return device.receive(frame); }
bool transmit(const u8* frame, u32 size) { return device.transmit(frame, size); }

}  // namespace mikos::drivers::net
