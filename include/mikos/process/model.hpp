#pragma once

#include <mikos/base.hpp>
#include <mikos/process/signal.hpp>

namespace mikos::process_model {

inline constexpr u32 invalid_pid = 0;

enum class ProcessState : u8 {
  free,
  runnable,
  running,
  waiting,
  vfork_wait,
  stopped,
  zombie,
};

enum class ProcessStatus : u8 {
  success,
  no_process,
  no_child,
  would_block,
  no_space,
  invalid_argument,
  permission_denied,
};

enum class WaitKind : u8 { any, pid, process_group };

struct WaitSelector {
  WaitKind kind{WaitKind::any};
  u32 value{};
};

struct ProcessRecord {
  ProcessState state{ProcessState::free};
  u32 pid{};
  u32 parent{};
  u32 process_group{};
  u32 session{};
  u32 exit_status{};
  u32 vfork_parent{};
  bool executed{};
  bool address_space{};
  SignalState signals{};
};

struct ForkResult {
  ProcessStatus status{ProcessStatus::no_space};
  u32 parent_result{};
  u32 child_result{};
  u32 child{};
};

struct WaitResult {
  ProcessStatus status{ProcessStatus::no_child};
  u32 pid{};
  u32 status_word{};
};

template <u32 Capacity = 8>
class ProcessTable {
 public:
  static_assert(Capacity >= 2);

  [[nodiscard]] constexpr ProcessStatus initialize(u32 pid = 1) {
    if (pid == invalid_pid || used() != 0) {
      return ProcessStatus::invalid_argument;
    }
    records_[0] = {ProcessState::running, pid, invalid_pid, pid, pid,
                   0, 0, false, true, {}};
    next_pid_ = pid + 1;
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr ForkResult fork(u32 parent_pid,
                                           bool vfork = false) {
    auto* parent = find(parent_pid);
    if (parent == nullptr || parent->state == ProcessState::zombie ||
        parent->state == ProcessState::free) {
      return {ProcessStatus::no_process, 0, 0, 0};
    }
    ProcessRecord* child = free_slot();
    if (child == nullptr || next_pid_ == invalid_pid) {
      return {};
    }
    const u32 pid = next_pid_++;
    *child = *parent;
    child->state = ProcessState::runnable;
    child->pid = pid;
    child->parent = parent_pid;
    child->exit_status = 0;
    child->vfork_parent = vfork ? parent_pid : 0;
    child->executed = false;
    child->address_space = true;
    child->signals = parent->signals;
    if (vfork) {
      parent->state = ProcessState::vfork_wait;
    }
    return {ProcessStatus::success, pid, 0, pid};
  }

  [[nodiscard]] constexpr ProcessStatus mark_exec(u32 pid) {
    auto* record = find(pid);
    if (record == nullptr || record->state == ProcessState::zombie) {
      return ProcessStatus::no_process;
    }
    record->executed = true;
    wake_vfork_parent(*record);
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr ProcessStatus exit(u32 pid, u32 status) {
    auto* record = find(pid);
    if (record == nullptr || record->state == ProcessState::zombie) {
      return ProcessStatus::no_process;
    }
    record->exit_status = status;
    record->state = ProcessState::zombie;
    record->address_space = false;
    wake_vfork_parent(*record);
    if (auto* parent = find(record->parent);
        parent != nullptr && parent->state == ProcessState::waiting) {
      parent->state = ProcessState::runnable;
    }
    reparent_children(pid);
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr WaitResult wait(u32 parent_pid,
                                           WaitSelector selector,
                                           bool no_hang = false) {
    auto* parent = find(parent_pid);
    if (parent == nullptr || parent->state == ProcessState::zombie) {
      return {ProcessStatus::no_process, 0, 0};
    }
    bool has_child = false;
    for (auto& candidate : records_) {
      if (candidate.state == ProcessState::free ||
          candidate.parent != parent_pid ||
          !matches(*parent, candidate, selector)) {
        continue;
      }
      has_child = true;
      if (candidate.state == ProcessState::zombie) {
        const WaitResult result{ProcessStatus::success, candidate.pid,
                                candidate.exit_status};
        candidate = {};
        return result;
      }
    }
    if (!has_child) {
      return {ProcessStatus::no_child, 0, 0};
    }
    if (no_hang) {
      return {ProcessStatus::success, 0, 0};
    }
    parent->state = ProcessState::waiting;
    return {ProcessStatus::would_block, 0, 0};
  }

  [[nodiscard]] constexpr ProcessStatus set_process_group(u32 caller_pid,
                                                           u32 target_pid,
                                                           u32 group) {
    auto* caller = find(caller_pid);
    if (caller == nullptr) {
      return ProcessStatus::no_process;
    }
    if (target_pid == 0) {
      target_pid = caller_pid;
    }
    auto* target = find(target_pid);
    if (target == nullptr ||
        (target_pid != caller_pid && target->parent != caller_pid)) {
      return ProcessStatus::no_process;
    }
    if (target->session == target->pid ||
        (target_pid != caller_pid && target->executed)) {
      return ProcessStatus::permission_denied;
    }
    if (group == 0) {
      group = target_pid;
    }
    if (group != target_pid) {
      const auto* leader = find(group);
      if (leader == nullptr || leader->process_group != group ||
          leader->session != target->session) {
        return ProcessStatus::permission_denied;
      }
    }
    target->process_group = group;
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr ProcessStatus create_session(u32 pid) {
    auto* record = find(pid);
    if (record == nullptr) {
      return ProcessStatus::no_process;
    }
    if (record->process_group == pid) {
      return ProcessStatus::permission_denied;
    }
    record->session = pid;
    record->process_group = pid;
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr ProcessStatus stop(u32 pid) {
    auto* record = find(pid);
    if (record == nullptr || record->state == ProcessState::zombie) {
      return ProcessStatus::no_process;
    }
    record->state = ProcessState::stopped;
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr ProcessStatus resume(u32 pid) {
    auto* record = find(pid);
    if (record == nullptr || record->state == ProcessState::zombie) {
      return ProcessStatus::no_process;
    }
    if (record->state == ProcessState::stopped) {
      record->state = ProcessState::runnable;
    }
    return ProcessStatus::success;
  }

  [[nodiscard]] constexpr u32 next_runnable(u32 after_pid) const {
    u32 after_slot = 0;
    bool found = false;
    for (u32 i = 0; i < Capacity; ++i) {
      if (records_[i].pid == after_pid &&
          records_[i].state != ProcessState::free) {
        after_slot = i;
        found = true;
        break;
      }
    }
    for (u32 offset = 1; offset <= Capacity; ++offset) {
      const u32 i = (after_slot + offset) % Capacity;
      if (runnable(records_[i].state)) {
        return records_[i].pid;
      }
    }
    if (!found && runnable(records_[0].state)) {
      return records_[0].pid;
    }
    return invalid_pid;
  }

  [[nodiscard]] constexpr ProcessRecord* find(u32 pid) {
    for (auto& record : records_) {
      if (record.state != ProcessState::free && record.pid == pid) {
        return &record;
      }
    }
    return nullptr;
  }

  [[nodiscard]] constexpr const ProcessRecord* find(u32 pid) const {
    for (const auto& record : records_) {
      if (record.state != ProcessState::free && record.pid == pid) {
        return &record;
      }
    }
    return nullptr;
  }

  [[nodiscard]] constexpr u32 used() const {
    u32 count = 0;
    for (const auto& record : records_) {
      count += record.state == ProcessState::free ? 0u : 1u;
    }
    return count;
  }

  [[nodiscard]] constexpr u32 address_spaces() const {
    u32 count = 0;
    for (const auto& record : records_) {
      count += record.address_space ? 1u : 0u;
    }
    return count;
  }

  [[nodiscard]] constexpr bool invariant() const {
    for (u32 i = 0; i < Capacity; ++i) {
      const auto& record = records_[i];
      if (record.state == ProcessState::free) {
        if (record.pid != 0 || record.address_space) {
          return false;
        }
        continue;
      }
      if (record.pid == 0 || record.process_group == 0 ||
          record.session == 0 ||
          (record.state == ProcessState::zombie && record.address_space)) {
        return false;
      }
      for (u32 j = i + 1; j < Capacity; ++j) {
        if (records_[j].state != ProcessState::free &&
            records_[j].pid == record.pid) {
          return false;
        }
      }
    }
    return true;
  }

 private:
  [[nodiscard]] static constexpr bool runnable(ProcessState state) {
    return state == ProcessState::runnable || state == ProcessState::running;
  }

  [[nodiscard]] constexpr ProcessRecord* free_slot() {
    for (auto& record : records_) {
      if (record.state == ProcessState::free) {
        return &record;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static constexpr bool matches(const ProcessRecord& parent,
                                               const ProcessRecord& child,
                                               WaitSelector selector) {
    switch (selector.kind) {
      case WaitKind::any:
        return true;
      case WaitKind::pid:
        return child.pid == selector.value;
      case WaitKind::process_group:
        return child.process_group ==
               (selector.value == 0 ? parent.process_group : selector.value);
    }
    return false;
  }

  constexpr void wake_vfork_parent(ProcessRecord& child) {
    if (child.vfork_parent == 0) {
      return;
    }
    if (auto* parent = find(child.vfork_parent);
        parent != nullptr && parent->state == ProcessState::vfork_wait) {
      parent->state = ProcessState::runnable;
    }
    child.vfork_parent = 0;
  }

  constexpr void reparent_children(u32 exiting) {
    u32 adopter = 0;
    for (const auto& record : records_) {
      if (record.state != ProcessState::free && record.pid == 1 &&
          record.pid != exiting) {
        adopter = 1;
        break;
      }
    }
    for (auto& record : records_) {
      if (record.state != ProcessState::free && record.parent == exiting) {
        record.parent = adopter;
      }
    }
  }

  ProcessRecord records_[Capacity]{};
  u32 next_pid_{1};
};

}  // namespace mikos::process_model
