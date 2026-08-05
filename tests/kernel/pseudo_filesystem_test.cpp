#include <kernel/pseudo_filesystem.hpp>

#include <support/test.hpp>

namespace {

using Filesystem = mikos::pseudo_fs::Filesystem;
using Node = Filesystem::Node;
using Type = Filesystem::Type;

[[nodiscard]] bool contains(const char* text, const char* needle) {
  if (text == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }
  for (mikos::u32 start = 0; text[start] != '\0'; ++start) {
    mikos::u32 index = 0;
    while (needle[index] != '\0' &&
           text[start + index] == needle[index]) {
      ++index;
    }
    if (needle[index] == '\0') {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool has_entry(Node directory, const char* name, Type type) {
  for (mikos::u32 index = 0;
       index < Filesystem::entry_count(directory); ++index) {
    const auto entry = Filesystem::entry(directory, index);
    const char* actual = entry.name;
    const char* expected = name;
    while (*actual == *expected && *actual != '\0') {
      ++actual;
      ++expected;
    }
    if (*actual == *expected && entry.type == type) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  mikos::test::Suite suite{"kernel/pseudo_filesystem"};

  MIKOS_CHECK(suite, Filesystem::lookup("/dev/null") == Node::dev_null);
  MIKOS_CHECK(suite, !Filesystem::directory(Node::dev_null));
  MIKOS_CHECK(suite, Filesystem::size(Node::dev_null) == 0);

  MIKOS_CHECK(suite, Filesystem::mounted("/proc"));
  MIKOS_CHECK(suite, Filesystem::mounted("/proc/net/tcp"));
  MIKOS_CHECK(suite, Filesystem::mounted("/sys/kernel"));
  MIKOS_CHECK(suite, !Filesystem::mounted("/procedure"));

  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/tcp") == Node::proc_net_tcp);
  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/dev") == Node::proc_net_dev);
  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/tcp6") == Node::proc_net_tcp6);
  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/udp") == Node::proc_net_udp);
  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/udp6") == Node::proc_net_udp6);
  MIKOS_CHECK(suite,
              Filesystem::lookup("/proc/net/raw") == Node::proc_net_raw);
  MIKOS_CHECK(suite, Filesystem::lookup("/proc/net/missing") == Node::none);

  MIKOS_CHECK(suite, Filesystem::directory(Node::proc));
  MIKOS_CHECK(suite, Filesystem::directory(Node::proc_net));
  MIKOS_CHECK(suite, !Filesystem::directory(Node::proc_net_tcp));
  MIKOS_CHECK(suite,
              has_entry(Node::proc, "net", Type::directory));
  MIKOS_CHECK(suite,
              has_entry(Node::proc_net, "tcp", Type::regular));
  MIKOS_CHECK(suite,
              has_entry(Node::proc_net, "dev", Type::regular));
  MIKOS_CHECK(suite,
              has_entry(Node::proc_net, "raw", Type::regular));
  MIKOS_CHECK(suite,
              contains(Filesystem::contents(Node::proc_net_tcp),
                       "local_address"));
  MIKOS_CHECK(suite, Filesystem::size(Node::proc_net_tcp) > 0);
  MIKOS_CHECK(suite,
              contains(Filesystem::contents(Node::proc_net_dev), "eth0:"));

  MIKOS_CHECK(suite, Filesystem::directory(Node::sys));
  MIKOS_CHECK(suite,
              Filesystem::lookup("/sys/devices/system/cpu/online") ==
                  Node::sys_cpu_online);
  MIKOS_CHECK(suite,
              has_entry(Node::sys_devices_system_cpu, "online",
                        Type::regular));
  MIKOS_CHECK(suite,
              contains(Filesystem::contents(Node::sys_kernel_ostype),
                       "Linux"));

  return suite.finish();
}
