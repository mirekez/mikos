#include <drivers/net/virtio.hpp>

#include <support/test.hpp>

int main() {
  mikos::test::Suite suite{"drivers/virtio"};
  using mikos::drivers::virtio::Feature;
  using mikos::drivers::virtio::FeatureSet;

  constexpr FeatureSet requested{Feature::mac, Feature::version_1};
  constexpr auto offered = FeatureSet::from_banks(0xffffffffu, 1);
  constexpr auto accepted = offered & requested;
  const auto absent = FeatureSet::from_banks(0, 0) & requested;

  MIKOS_CHECK(suite, requested.low() == 1u << 5);
  MIKOS_CHECK(suite, requested.high() == 1);
  MIKOS_CHECK(suite, requested.contains(Feature::mac));
  MIKOS_CHECK(suite, requested.contains(Feature::version_1));
  MIKOS_CHECK(suite, accepted.low() == 1u << 5);
  MIKOS_CHECK(suite, accepted.high() == 1);
  MIKOS_CHECK(suite, !absent.contains(Feature::mac));

  mikos::drivers::virtio::SplitQueue<4> queue;
  queue.reset();
  mikos::u8 first[4]{};
  mikos::u8 second[8]{};
  queue.describe(0, first, sizeof(first),
                 mikos::drivers::virtio::Access::device_reads, 1);
  queue.describe(1, second, sizeof(second),
                 mikos::drivers::virtio::Access::device_writes);
  MIKOS_CHECK(suite, queue.descriptor(0).flags == 1);
  MIKOS_CHECK(suite, queue.descriptor(0).next == 1);
  MIKOS_CHECK(suite, queue.descriptor(1).flags == 2);
  MIKOS_CHECK(suite, queue.descriptor(1).next == 0);

  return suite.finish();
}
