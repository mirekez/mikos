#pragma once

#include <mikos/base.hpp>

namespace mikos::pseudo_fs {

class Filesystem {
 public:
  enum class Node : u8 {
    none,
    dev_null,
    proc,
    proc_pid1,
    proc_pid2,
    proc_net,
    proc_stat,
    proc_meminfo,
    proc_loadavg,
    proc_uptime,
    proc_version,
    proc_filesystems,
    proc_mounts,
    proc_pid1_stat,
    proc_pid2_stat,
    proc_pid1_cmdline,
    proc_pid2_cmdline,
    proc_net_tcp,
    proc_net_tcp6,
    proc_net_udp,
    proc_net_udp6,
    proc_net_raw,
    proc_net_raw6,
    proc_net_unix,
    sys,
    sys_class,
    sys_devices,
    sys_devices_system,
    sys_devices_system_cpu,
    sys_kernel,
    sys_cpu_online,
    sys_cpu_present,
    sys_cpu_possible,
    sys_kernel_osrelease,
    sys_kernel_ostype,
    sys_kernel_hostname,
  };

  enum class Type : u8 { none, directory, regular };

  struct Entry {
    const char* name;
    Node node;
    Type type;
  };

  [[nodiscard]] static Node lookup(const char* path);
  [[nodiscard]] static bool mounted(const char* path);
  [[nodiscard]] static bool directory(Node node);
  [[nodiscard]] static u32 inode(Node node);
  [[nodiscard]] static const char* contents(Node node);
  [[nodiscard]] static u32 size(Node node);
  [[nodiscard]] static u32 entry_count(Node directory);
  [[nodiscard]] static Entry entry(Node directory, u32 index);
};

}  // namespace mikos::pseudo_fs
