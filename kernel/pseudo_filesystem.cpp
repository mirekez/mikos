#include <kernel/pseudo_filesystem.hpp>

namespace mikos::pseudo_fs {
namespace {

using Node = Filesystem::Node;
using Type = Filesystem::Type;
using Entry = Filesystem::Entry;

[[nodiscard]] bool text_is(const char* actual, const char* expected) {
  if (actual == nullptr || expected == nullptr) {
    return false;
  }
  while (*actual == *expected) {
    if (*actual == '\0') {
      return true;
    }
    ++actual;
    ++expected;
  }
  return false;
}

[[nodiscard]] bool path_below(const char* path, const char* mount) {
  if (path == nullptr) {
    return false;
  }
  while (*mount != '\0' && *path == *mount) {
    ++path;
    ++mount;
  }
  return *mount == '\0' && (*path == '\0' || *path == '/');
}

[[nodiscard]] u32 text_size(const char* text) {
  u32 result = 0;
  if (text != nullptr) {
    while (text[result] != '\0') {
      ++result;
    }
  }
  return result;
}

struct PathNode {
  const char* path;
  Node node;
};

constexpr PathNode nodes[] = {
    {"/dev/null", Node::dev_null},
    {"/proc", Node::proc},
    {"/proc/1", Node::proc_pid1},
    {"/proc/2", Node::proc_pid2},
    {"/proc/net", Node::proc_net},
    {"/proc/stat", Node::proc_stat},
    {"/proc/meminfo", Node::proc_meminfo},
    {"/proc/loadavg", Node::proc_loadavg},
    {"/proc/uptime", Node::proc_uptime},
    {"/proc/version", Node::proc_version},
    {"/proc/filesystems", Node::proc_filesystems},
    {"/proc/mounts", Node::proc_mounts},
    {"/proc/1/stat", Node::proc_pid1_stat},
    {"/proc/2/stat", Node::proc_pid2_stat},
    {"/proc/1/cmdline", Node::proc_pid1_cmdline},
    {"/proc/2/cmdline", Node::proc_pid2_cmdline},
    {"/proc/net/tcp", Node::proc_net_tcp},
    {"/proc/net/tcp6", Node::proc_net_tcp6},
    {"/proc/net/udp", Node::proc_net_udp},
    {"/proc/net/udp6", Node::proc_net_udp6},
    {"/proc/net/raw", Node::proc_net_raw},
    {"/proc/net/raw6", Node::proc_net_raw6},
    {"/proc/net/unix", Node::proc_net_unix},
    {"/sys", Node::sys},
    {"/sys/class", Node::sys_class},
    {"/sys/devices", Node::sys_devices},
    {"/sys/devices/system", Node::sys_devices_system},
    {"/sys/devices/system/cpu", Node::sys_devices_system_cpu},
    {"/sys/kernel", Node::sys_kernel},
    {"/sys/devices/system/cpu/online", Node::sys_cpu_online},
    {"/sys/devices/system/cpu/present", Node::sys_cpu_present},
    {"/sys/devices/system/cpu/possible", Node::sys_cpu_possible},
    {"/sys/kernel/osrelease", Node::sys_kernel_osrelease},
    {"/sys/kernel/ostype", Node::sys_kernel_ostype},
    {"/sys/kernel/hostname", Node::sys_kernel_hostname},
};

constexpr Entry proc_entries[] = {
    {".", Node::proc, Type::directory},
    {"..", Node::proc, Type::directory},
    {"1", Node::proc_pid1, Type::directory},
    {"2", Node::proc_pid2, Type::directory},
    {"net", Node::proc_net, Type::directory},
    {"stat", Node::proc_stat, Type::regular},
    {"meminfo", Node::proc_meminfo, Type::regular},
    {"loadavg", Node::proc_loadavg, Type::regular},
    {"uptime", Node::proc_uptime, Type::regular},
    {"version", Node::proc_version, Type::regular},
    {"filesystems", Node::proc_filesystems, Type::regular},
    {"mounts", Node::proc_mounts, Type::regular},
};

constexpr Entry pid1_entries[] = {
    {".", Node::proc_pid1, Type::directory},
    {"..", Node::proc, Type::directory},
    {"stat", Node::proc_pid1_stat, Type::regular},
    {"cmdline", Node::proc_pid1_cmdline, Type::regular},
};

constexpr Entry pid2_entries[] = {
    {".", Node::proc_pid2, Type::directory},
    {"..", Node::proc, Type::directory},
    {"stat", Node::proc_pid2_stat, Type::regular},
    {"cmdline", Node::proc_pid2_cmdline, Type::regular},
};

constexpr Entry net_entries[] = {
    {".", Node::proc_net, Type::directory},
    {"..", Node::proc, Type::directory},
    {"tcp", Node::proc_net_tcp, Type::regular},
    {"tcp6", Node::proc_net_tcp6, Type::regular},
    {"udp", Node::proc_net_udp, Type::regular},
    {"udp6", Node::proc_net_udp6, Type::regular},
    {"raw", Node::proc_net_raw, Type::regular},
    {"raw6", Node::proc_net_raw6, Type::regular},
    {"unix", Node::proc_net_unix, Type::regular},
};

constexpr Entry sys_entries[] = {
    {".", Node::sys, Type::directory},
    {"..", Node::sys, Type::directory},
    {"class", Node::sys_class, Type::directory},
    {"devices", Node::sys_devices, Type::directory},
    {"kernel", Node::sys_kernel, Type::directory},
};

constexpr Entry empty_directory_entries[] = {
    {".", Node::sys_class, Type::directory},
    {"..", Node::sys, Type::directory},
};

constexpr Entry devices_entries[] = {
    {".", Node::sys_devices, Type::directory},
    {"..", Node::sys, Type::directory},
    {"system", Node::sys_devices_system, Type::directory},
};

constexpr Entry system_entries[] = {
    {".", Node::sys_devices_system, Type::directory},
    {"..", Node::sys_devices, Type::directory},
    {"cpu", Node::sys_devices_system_cpu, Type::directory},
};

constexpr Entry cpu_entries[] = {
    {".", Node::sys_devices_system_cpu, Type::directory},
    {"..", Node::sys_devices_system, Type::directory},
    {"online", Node::sys_cpu_online, Type::regular},
    {"present", Node::sys_cpu_present, Type::regular},
    {"possible", Node::sys_cpu_possible, Type::regular},
};

constexpr Entry kernel_entries[] = {
    {".", Node::sys_kernel, Type::directory},
    {"..", Node::sys, Type::directory},
    {"osrelease", Node::sys_kernel_osrelease, Type::regular},
    {"ostype", Node::sys_kernel_ostype, Type::regular},
    {"hostname", Node::sys_kernel_hostname, Type::regular},
};

struct DirectoryView {
  const Entry* entries;
  u32 size;
};

template <usize Size>
[[nodiscard]] constexpr DirectoryView view(const Entry (&entries)[Size]) {
  return {entries, static_cast<u32>(Size)};
}

[[nodiscard]] DirectoryView directory_view(Node node) {
  switch (node) {
    case Node::proc:
      return view(proc_entries);
    case Node::proc_pid1:
      return view(pid1_entries);
    case Node::proc_pid2:
      return view(pid2_entries);
    case Node::proc_net:
      return view(net_entries);
    case Node::sys:
      return view(sys_entries);
    case Node::sys_class:
      return view(empty_directory_entries);
    case Node::sys_devices:
      return view(devices_entries);
    case Node::sys_devices_system:
      return view(system_entries);
    case Node::sys_devices_system_cpu:
      return view(cpu_entries);
    case Node::sys_kernel:
      return view(kernel_entries);
    default:
      return {nullptr, 0};
  }
}

}  // namespace

Filesystem::Node Filesystem::lookup(const char* path) {
  for (const auto& candidate : nodes) {
    if (text_is(path, candidate.path)) {
      return candidate.node;
    }
  }
  return Node::none;
}

bool Filesystem::mounted(const char* path) {
  return path_below(path, "/proc") || path_below(path, "/sys");
}

bool Filesystem::directory(Node node) {
  return directory_view(node).entries != nullptr;
}

u32 Filesystem::inode(Node node) {
  // Keep virtual inode numbers away from small on-disk inode numbers.
  return node == Node::none ? 0 : 0x70000000u + static_cast<u32>(node);
}

const char* Filesystem::contents(Node node) {
#ifdef MIKOS_TRIBE_MULTICORE
  static constexpr const char cpu[] =
      "cpu  400 0 400 40000 0 0 0 0\n"
      "cpu0 100 0 100 10000 0 0 0 0\n"
      "cpu1 100 0 100 10000 0 0 0 0\n"
      "cpu2 100 0 100 10000 0 0 0 0\n"
      "cpu3 100 0 100 10000 0 0 0 0\n";
  static constexpr const char cpu_set[] = "0-3\n";
#else
  static constexpr const char cpu[] =
      "cpu  100 0 100 10000 0 0 0 0\n"
      "cpu0 100 0 100 10000 0 0 0 0\n";
  static constexpr const char cpu_set[] = "0\n";
#endif
  static constexpr const char memory[] =
      "MemTotal:       16384 kB\n"
      "MemFree:         4096 kB\n"
      "MemShared:          0 kB\n"
      "Buffers:          128 kB\n"
      "Cached:          2048 kB\n"
      "SReclaimable:       0 kB\n"
      "Shmem:              0 kB\n"
      "SwapTotal:          0 kB\n"
      "SwapFree:           0 kB\n";
  static constexpr const char load[] = "0.00 0.00 0.00 1/2 2\n";
  static constexpr const char uptime[] = "0.00 0.00\n";
  static constexpr const char version[] =
      "Linux version 0.1 (MikOS) #1 riscv32\n";
  static constexpr const char filesystems[] =
      "nodev\tproc\n"
      "nodev\tsysfs\n"
      "\text4\n"
      "\tvfat\n";
  static constexpr const char mounts[] =
      "/dev/root / ext4 rw 0 0\n"
      "proc /proc proc ro 0 0\n"
      "sysfs /sys sysfs ro 0 0\n";
  static constexpr const char shell_stat[] =
      "1 (sh) S 0 1 1 0 0 0 0 0 0 0 1 1 0 0 20 0 1 0 1 2500000 128 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  static constexpr const char top_stat[] =
      "2 (top) R 1 2 2 0 0 0 0 0 0 0 2 1 0 0 20 0 1 0 1 2600000 144 "
      "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
  static constexpr const char shell_command[] = "sh\0";
  static constexpr const char top_command[] = "top\0";
  static constexpr const char inet_sockets[] =
      "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
      "retrnsmt   uid  timeout inode\n";
  static constexpr const char unix_sockets[] =
      "Num       RefCount Protocol Flags    Type St Inode Path\n";
  static constexpr const char osrelease[] = "0.1\n";
  static constexpr const char ostype[] = "Linux\n";
  static constexpr const char hostname[] = "mikos\n";

  switch (node) {
    case Node::proc_stat:
      return cpu;
    case Node::proc_meminfo:
      return memory;
    case Node::proc_loadavg:
      return load;
    case Node::proc_uptime:
      return uptime;
    case Node::proc_version:
      return version;
    case Node::proc_filesystems:
      return filesystems;
    case Node::proc_mounts:
      return mounts;
    case Node::proc_pid1_stat:
      return shell_stat;
    case Node::proc_pid2_stat:
      return top_stat;
    case Node::proc_pid1_cmdline:
      return shell_command;
    case Node::proc_pid2_cmdline:
      return top_command;
    case Node::proc_net_tcp:
    case Node::proc_net_tcp6:
    case Node::proc_net_udp:
    case Node::proc_net_udp6:
    case Node::proc_net_raw:
    case Node::proc_net_raw6:
      return inet_sockets;
    case Node::proc_net_unix:
      return unix_sockets;
    case Node::sys_cpu_online:
    case Node::sys_cpu_present:
    case Node::sys_cpu_possible:
      return cpu_set;
    case Node::sys_kernel_osrelease:
      return osrelease;
    case Node::sys_kernel_ostype:
      return ostype;
    case Node::sys_kernel_hostname:
      return hostname;
    default:
      return nullptr;
  }
}

u32 Filesystem::size(Node node) {
  if (node == Node::proc_pid1_cmdline) {
    return 3;
  }
  if (node == Node::proc_pid2_cmdline) {
    return 4;
  }
  return text_size(contents(node));
}

u32 Filesystem::entry_count(Node directory) {
  return directory_view(directory).size;
}

Filesystem::Entry Filesystem::entry(Node directory, u32 index) {
  const auto selected = directory_view(directory);
  return index < selected.size ? selected.entries[index]
                               : Entry{nullptr, Node::none, Type::none};
}

}  // namespace mikos::pseudo_fs
