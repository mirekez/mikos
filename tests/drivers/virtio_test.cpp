#include <mikos/drivers/virtio.hpp>

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

  return suite.finish();
}
