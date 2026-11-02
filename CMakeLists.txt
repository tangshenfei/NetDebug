cmake_minimum_required(VERSION 3.20)
project(netdebug VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(CRYPTO_BACKEND "WinCNG" CACHE STRING "" FORCE)
    add_subdirectory(third_party/libssh2-1.11.1 EXCLUDE_FROM_ALL)
endif()

set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libsmb2-libsmb2-6.3 EXCLUDE_FROM_ALL)
if(WIN32)
    target_compile_definitions(smb2 PRIVATE _WINDOWS)
endif()

set(ND_LIB_SOURCES
    src/core/error.c
    src/core/log.c
    src/core/thread.c
    src/core/fqueue.c
    src/core/ring.c
    src/core/util.c
    src/platform/platform.c
    src/platform/gui.c
    src/platform/netio.c
    src/platform/serial.c
    src/net/addr.c
    src/net/iface.c
    src/net/planner.c
    src/net/bind_json.c
    src/net/resolver.c
    src/net/url.c
    src/session/conn.c
    src/session/service.c
    src/session/session.c
    src/session/store.c
    src/session/ftp.c
    src/session/smb.c
    src/proto/telnet.c
    src/proto/http_client.c
    src/proto/http_server.c
    src/proto/ftp_client.c
    src/proto/ftp_server.c
    src/proto/tftp_client.c
    src/proto/tftp_server.c
    src/proto/ssh_client.c
    src/proto/smb2_client.c
    src/httpd/http.c
    src/httpd/sha1.c
    src/httpd/base64.c
    src/httpd/ws.c
    src/httpd/server.c
    src/httpd/api.c
    third_party/cjson/cJSON.c
)

if(WIN32)
    list(APPEND ND_LIB_SOURCES src/platform/webview.cpp)
endif()

add_library(ndlib STATIC ${ND_LIB_SOURCES})
target_include_directories(ndlib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/cjson
    ${CMAKE_SOURCE_DIR}/third_party/libsmb2-libsmb2-6.3/include
)
if(WIN32)
    target_include_directories(ndlib PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/webview2/build/native/include)
endif()
if(WIN32)
    target_compile_definitions(ndlib PUBLIC ND_HAVE_SSH=1)
    target_link_libraries(ndlib PUBLIC libssh2_static)
endif()
target_compile_definitions(ndlib PUBLIC ND_HAVE_SMB2=1)
target_link_libraries(ndlib PUBLIC smb2)

if(WIN32)
    target_link_libraries(ndlib PUBLIC ws2_32 iphlpapi bcrypt shell32 setupapi advapi32 user32 gdi32 dwmapi comctl32 ole32 oleaut32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ndlib PUBLIC Threads::Threads)

    # Linux 图形界面：GTK3 + WebKitGTK，与 Windows 的 WebView2 宿主对应
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK3 QUIET gtk+-3.0)
        pkg_check_modules(WEBKIT2 QUIET webkit2gtk-4.1)
    endif()
    if(GTK3_FOUND AND WEBKIT2_FOUND)
        message(STATUS "Linux GUI: GTK ${GTK3_VERSION} + WebKitGTK ${WEBKIT2_VERSION}")
        target_sources(ndlib PRIVATE src/platform/gui_gtk.c)
        target_compile_definitions(ndlib PUBLIC ND_HAVE_GTK=1)
        target_include_directories(ndlib PUBLIC ${GTK3_INCLUDE_DIRS} ${WEBKIT2_INCLUDE_DIRS})
        target_link_directories(ndlib PUBLIC ${GTK3_LIBRARY_DIRS} ${WEBKIT2_LIBRARY_DIRS})
        target_link_libraries(ndlib PUBLIC ${GTK3_LIBRARIES} ${WEBKIT2_LIBRARIES})
    else()
        message(STATUS "Linux GUI: not available (install libgtk-3-dev libwebkit2gtk-4.1-dev)")
    endif()
endif()

file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/web/*)
set(WEB_ASSETS_C ${CMAKE_BINARY_DIR}/web_assets.c)
add_custom_command(
    OUTPUT ${WEB_ASSETS_C}
    COMMAND ${CMAKE_COMMAND}
            -DWEB_DIR=${CMAKE_SOURCE_DIR}/web
            -DOUT=${WEB_ASSETS_C}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding web assets"
)
add_custom_target(web_assets DEPENDS ${WEB_ASSETS_C})

add_executable(netdebug src/main.c ${WEB_ASSETS_C})
add_dependencies(netdebug web_assets)
if(MINGW)
    target_link_options(netdebug PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(WIN32)
    enable_language(RC)
    configure_file(res/netdebug.rc.in ${CMAKE_BINARY_DIR}/netdebug.rc @ONLY)
    target_sources(netdebug PRIVATE ${CMAKE_BINARY_DIR}/netdebug.rc)
    target_link_libraries(netdebug PRIVATE dwmapi)
endif()
target_link_libraries(netdebug PRIVATE ndlib)

enable_testing()
add_subdirectory(tests)
