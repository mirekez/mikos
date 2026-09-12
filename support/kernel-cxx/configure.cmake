# Configure the pinned library headers for the kernel, independently of the host.
set(_LIBCPP_ABI_VERSION 1)
set(_LIBCPP_ABI_NAMESPACE __1)
set(_LIBCPP_PSTL_BACKEND_SERIAL ON)
set(_LIBCPP_HARDENING_MODE_DEFAULT 4) # libc++ fast hardening
set(_LIBCPP_HAS_NO_STD_MODULES ON)
configure_file("${SOURCE}/include/__config_site.in"
               "${OUTPUT}/__config_site" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/assertion_handler.hpp"
               "${OUTPUT}/__assertion_handler" COPYONLY)
file(WRITE "${OUTPUT}/beman/inplace_vector/config_generated.hpp"
     "#pragma once\n#define BEMAN_INPLACE_VECTOR_NO_EXCEPTIONS() 1\n")
