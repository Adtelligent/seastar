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

find_path (LinuxMembarrier_INCLUDE_DIR
  NAMES linux/membarrier.h)

file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/LinuxMembarrier_test.cc _linuxmembarrier_test_code)

# Use try_compile instead of check_cxx_source_compiles for better reliability
# with CMake 3.25+ where check_cxx_source_compiles can fail silently even
# when the code compiles successfully
set(_test_dir "${CMAKE_BINARY_DIR}/CMakeFiles/LinuxMembarrierTest")
file(MAKE_DIRECTORY "${_test_dir}")
file(WRITE "${_test_dir}/test.cpp" "${_linuxmembarrier_test_code}")

try_compile(
  LinuxMembarrier_FOUND
  "${_test_dir}"
  "${_test_dir}/test.cpp"
  OUTPUT_VARIABLE _compile_output
)

if(NOT LinuxMembarrier_FOUND)
  message(STATUS "LinuxMembarrier compilation failed. Output:\n${_compile_output}")
endif()

unset(_test_dir)
unset(_compile_output)

if (LinuxMembarrier_FOUND)
  set (LinuxMembarrier_INCLUDE_DIRS ${LinuxMembarrier_INCLUDE_DIR})
endif ()

if (LinuxMembarrier_FOUND AND NOT (TARGET LinuxMembarrier::membarrier))
  add_library (LinuxMembarrier::membarrier INTERFACE IMPORTED)

  set_target_properties (LinuxMembarrier::membarrier
    PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES ${LinuxMembarrier_INCLUDE_DIRS})
endif ()
