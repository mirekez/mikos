#pragma once

#include <mikos/base.hpp>

namespace mikos::drivers::uart {

[[nodiscard]] bool initialize();
void put(u8 value);
[[nodiscard]] bool ready();
[[nodiscard]] bool receive(u8& value);

}  // namespace mikos::drivers::uart
