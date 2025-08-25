# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/ncs/v2.9.1/Vitals_monitoring_system")
  file(MAKE_DIRECTORY "C:/ncs/v2.9.1/Vitals_monitoring_system")
endif()
file(MAKE_DIRECTORY
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/Vitals_monitoring_system"
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix"
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/tmp"
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/src/Vitals_monitoring_system-stamp"
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/src"
  "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/src/Vitals_monitoring_system-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/src/Vitals_monitoring_system-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/ncs/v2.9.1/Vitals_monitoring_system/build/_sysbuild/sysbuild/images/Vitals_monitoring_system-prefix/src/Vitals_monitoring_system-stamp${cfgdir}") # cfgdir has leading slash
endif()
