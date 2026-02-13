#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Copyright (C) 2018 Scylladb, Ltd.
#

find_package (PkgConfig REQUIRED)
pkg_check_modules (dpdk_PC libdpdk)

# we cannot use ${dpdk_PC_STATIC_LDFLAGS} directly, because we want to
# export DPDK as a bundle of static libraries, so need to find the
# individual paths to all .a files

# Determine whether to use static or dynamic linking
# Default to static for backwards compatibility
if (NOT DEFINED Seastar_DPDK_STATIC)
  set(Seastar_DPDK_STATIC ON)
endif()

# Use include dirs from pkg-config first
if (dpdk_PC_INCLUDE_DIRS)
  # pkg-config provides include directories, find the one with rte_ethdev.h
  foreach(inc_dir ${dpdk_PC_INCLUDE_DIRS})
    if (EXISTS "${inc_dir}/rte_ethdev.h")
      set(dpdk_INCLUDE_DIR "${inc_dir}")
      message(STATUS "Found DPDK headers in: ${dpdk_INCLUDE_DIR}")
      break()
    endif()
  endforeach()
endif()

# Fallback to find_path if pkg-config didn't provide valid path
if (NOT dpdk_INCLUDE_DIR)
  find_path (dpdk_INCLUDE_DIR
    NAMES rte_ethdev.h
    HINTS
      ${dpdk_PC_INCLUDE_DIRS}
    PATH_SUFFIXES
      dpdk)
endif()

if (dpdk_INCLUDE_DIR AND EXISTS "${dpdk_INCLUDE_DIR}/rte_build_config.h")
  file (STRINGS "${dpdk_INCLUDE_DIR}/rte_build_config.h" rte_mbuf_refcnt_atomic
    REGEX "^#define[ \t ]+RTE_MBUF_REFCNT_ATOMIC")
  if (rte_mbuf_refcnt_atomic)
    message (WARNING
      "DPDK is configured with RTE_MBUF_REFCNT_ATOMIC enabled, "
      "please disable this option and recompile DPDK for better performance.")
  endif ()
endif ()

# Core DPDK libraries that are required
set(rte_libs_required
  eal
  ethdev
  mbuf
  mempool
  ring)

# Optional DPDK libraries and drivers
set(rte_libs_optional
  bus_pci
  bus_vdev
  cfgfile
  cmdline
  cryptodev
  hash
  kvargs
  mempool_ring
  net
  net_bnxt
  net_cxgbe
  net_e1000
  net_ena
  net_enic
  net_i40e
  net_ixgbe
  net_nfp
  net_qede
  net_ring
  net_sfc
  net_vmxnet3
  pci
  rcu
  security
  telemetry
  timer)
# sfc_efx driver can only build on x86 and aarch64
if (CMAKE_SYSTEM_PROCESSOR MATCHES "amd64|x86_64|aarch64")
  list (APPEND rte_libs_optional
    common_sfc_efx)
endif ()

set(rte_libs ${rte_libs_required} ${rte_libs_optional})

list (APPEND dpdk_REQUIRED
  dpdk_INCLUDE_DIR)

# we prefer static library over the shared library, so just find the
# static libraries first.
set (_cmake_find_library_suffixes_saved ${CMAKE_FIND_LIBRARY_SUFFIXES})
set (CMAKE_FIND_LIBRARY_SUFFIXES
  ${CMAKE_STATIC_LIBRARY_SUFFIX}
  ${CMAKE_SHARED_LIBRARY_SUFFIX})

foreach (lib ${rte_libs})
  string(TOUPPER ${lib} upper_lib)
  set(library_name "dpdk_${upper_lib}_LIBRARY")
  find_library (${library_name}
    NAME rte_${lib}
    HINTS
      ${dpdk_PC_STATIC_LIBRARY_DIRS})

  # Only mark required libraries as REQUIRED, others are optional
  if (lib IN_LIST rte_libs_required)
    list (APPEND dpdk_REQUIRED ${library_name})
  endif()

  list (APPEND dpdk_LIBRARIES
    ${library_name})

  if (NOT ${library_name})
    continue()
  endif ()

  set (library_path ${${library_name}})
  list (APPEND _dpdk_linker_files ${library_path})
  set (dpdk_lib dpdk::${lib})
  list (APPEND _dpdk_libraries ${dpdk_lib})

  if (dpdk_INCLUDE_DIR AND NOT (TARGET ${dpdk_lib}))
    add_library (${dpdk_lib} UNKNOWN IMPORTED)
    set_target_properties (${dpdk_lib}
      PROPERTIES
        IMPORTED_LOCATION ${library_path}
        INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})
  endif ()
endforeach ()

# restore the previous saved suffixes
set (CMAKE_FIND_LIBRARY_SUFFIXES ${_cmake_find_library_suffixes_saved})

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (dpdk
  REQUIRED_VARS
    ${dpdk_REQUIRED})

# DPDK's build system adds certain dependencies conditionally based on what's available
# at build time. While most libraries from dpdk_PC_LIBRARIES are handled through the
# rte_libs logic elsewhere, external dependencies ('bsd' and 'numa' in this case) are
# explicitly handled below. This foreach loop checks if these specific libraries are
# present in dpdk_PC_LIBRARIES and adds them to the dpdk_dependencies list if found.
foreach (lib "bsd" "numa")
  if (lib IN_LIST dpdk_PC_STATIC_LIBRARIES)
    list (APPEND dpdk_dependencies ${lib})
  endif()
endforeach ()

# As of DPDK 23.07, if libarchive-dev is present, it will make DPDK depend on the library.
# Unfortunately DPDK also has a bug in its .pc file generation and will not include libarchive
# dependency under any circumstance. Accordingly, the dependency is added explicitly if libarchive
# exists.
pkg_check_modules (libarchive_PC QUIET libarchive)
list(APPEND dpdk_dependencies ${libarchive_PC_LIBRARIES})

if (dpdk_FOUND AND NOT (TARGET dpdk))
  get_filename_component (library_suffix "${dpdk_EAL_LIBRARY}" LAST_EXT)
  # strictly speaking, we should have being using check_c_compiler_flag()
  # here, but we claim Seastar as a project written in CXX language, and
  # C is not enabled, so CXX is used here instead.
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-Wno-volatile" _warning_supported_volatile)
  if(_warning_supported_volatile)
    # include/generic/rte_spinlock.h increments volatiled-qualified type with
    # "++". but this is deprecated by GCC, so silence it.
    set(compile_options
      INTERFACE_COMPILE_OPTIONS "-Wno-volatile")
  endif()
  if (library_suffix STREQUAL CMAKE_STATIC_LIBRARY_SUFFIX AND Seastar_DPDK_STATIC)
    # No pmd driver code will be pulled in without "--whole-archive". To
    # avoid exposing that to seastar users, combine dpdk into a single
    # .o file.
    set (dpdk_object_path "${CMAKE_BINARY_DIR}/dpdk.o")
    add_custom_command (
      OUTPUT ${dpdk_object_path}
      COMMAND ${CMAKE_CXX_COMPILER}
        -r # create a relocatable object
        -o ${dpdk_object_path}
        -Wl,--whole-archive ${_dpdk_linker_files}
      DEPENDS
        ${_dpdk_linker_files})
    add_custom_target (dpdk_object
      DEPENDS ${dpdk_object_path})

    add_library (dpdk OBJECT IMPORTED)
    add_dependencies (dpdk dpdk_object)

    # Extract compile flags from pkg-config (like -include rte_config.h)
    set(dpdk_compile_flags "")
    if(dpdk_PC_CFLAGS_OTHER)
      list(APPEND dpdk_compile_flags ${dpdk_PC_CFLAGS_OTHER})
    endif()

    set(_dpdk_include_dirs ${dpdk_INCLUDE_DIR})
    if(dpdk_PC_INCLUDE_DIRS)
      set(_dpdk_include_dirs ${dpdk_PC_INCLUDE_DIRS})
    endif()

    set_target_properties (dpdk
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_dpdk_include_dirs}"
        INTERFACE_LINK_LIBRARIES "${dpdk_dependencies}"
        INTERFACE_COMPILE_OPTIONS "${dpdk_compile_flags}"
        IMPORTED_OBJECTS ${dpdk_object_path}
        ${compile_options})
    # we include dpdk in seastar already, but we need to pull in the
    # dependency libraries linked by dpdk
    list(TRANSFORM dpdk_dependencies PREPEND "-l" OUTPUT_VARIABLE dpdk_LIBRARIES)
    add_library (DPDK::dpdk ALIAS dpdk)
  else ()
    # Use static or dynamic linking based on Seastar_DPDK_STATIC
    if (Seastar_DPDK_STATIC)
      message(STATUS "Using static DPDK libraries")
      set (dpdk_LIBRARIES ${dpdk_PC_STATIC_LDFLAGS})
    else()
      message(STATUS "Using dynamic DPDK libraries")
      set (dpdk_LIBRARIES ${dpdk_PC_LDFLAGS})
    endif()

    add_library (DPDK::dpdk INTERFACE IMPORTED)

    # Extract compile flags from pkg-config (like -include rte_config.h)
    set(dpdk_compile_flags "")
    if(dpdk_PC_CFLAGS_OTHER)
      list(APPEND dpdk_compile_flags ${dpdk_PC_CFLAGS_OTHER})
    endif()

    set_target_properties (DPDK::dpdk
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${dpdk_PC_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${_dpdk_libraries};${dpdk_dependencies}"
        INTERFACE_COMPILE_OPTIONS "${dpdk_compile_flags}"
        ${compile_options})
  endif()

  # Export compile flags as a public variable for Seastar to use
  if(dpdk_PC_CFLAGS_OTHER)
    set(dpdk_CFLAGS ${dpdk_PC_CFLAGS_OTHER} CACHE STRING "DPDK compile flags")
  endif()
endif ()
