#pragma once
#include <__verbose_abort>
#define _LIBCPP_ASSERTION_HANDLER(message) _LIBCPP_VERBOSE_ABORT("%s", message)
