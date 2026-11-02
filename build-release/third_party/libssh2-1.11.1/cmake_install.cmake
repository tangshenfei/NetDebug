# Install script for directory: C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/netdebug")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tools/msys64/mingw64/bin/objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/libssh2" TYPE FILE FILES
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/COPYING"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/NEWS"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/README"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/RELEASE-NOTES"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/docs/AUTHORS"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/docs/BINDINGS.md"
    "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/third_party/libssh2-1.11.1/docs/HACKING.md"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/third_party/libssh2-1.11.1/src/cmake_install.cmake")
  include("C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/third_party/libssh2-1.11.1/docs/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/third_party/libssh2-1.11.1/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
