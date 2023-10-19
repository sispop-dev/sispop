
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      message(STATUS "Enabling ccache for ${lang}")
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    endif()
  endforeach()
endif()

cmake_minimum_required(VERSION 3.7)

# Has to be set before `project()`, and ignored on non-macos:
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target (Apple clang only)")

project(libsispopmq
    VERSION 1.2.5
    LANGUAGES CXX C)

include(GNUInstallDirs)

message(STATUS "sispopmq v${PROJECT_VERSION}")

set(SISPOPMQ_LIBVERSION 0)


if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(sispopmq_IS_TOPLEVEL_PROJECT TRUE)
else()
    set(sispopmq_IS_TOPLEVEL_PROJECT FALSE)
endif()


option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)
set(sispopmq_INSTALL_DEFAULT OFF)
if(BUILD_SHARED_LIBS OR sispopmq_IS_TOPLEVEL_PROJECT)
    set(sispopmq_INSTALL_DEFAULT ON)
endif()
option(SISPOPMQ_BUILD_TESTS "Building and perform sispopmq tests" ${sispopmq_IS_TOPLEVEL_PROJECT})
option(SISPOPMQ_INSTALL "Add sispopmq libraries and headers to cmake install target; defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project; OFF for a static subdirectory build" ${sispopmq_INSTALL_DEFAULT})
option(SISPOPMQ_INSTALL_CPPZMQ "Install cppzmq header with sispopmq/ headers (requires SISPOPMQ_INSTALL)" ON)
option(SISPOPMQ_LOKIMQ_COMPAT "Install lokimq compatibility headers and pkg-config for rename migration" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(sispopmq/version.h.in sispopmq/version.h @ONLY)
configure_file(libsispopmq.pc.in libsispopmq.pc @ONLY)
if(SISPOPMQ_LOKIMQ_COMPAT)
    configure_file(liblokimq.pc.in liblokimq.pc @ONLY)
endif()


add_library(sispopmq
    sispopmq/address.cpp
    sispopmq/auth.cpp
    sispopmq/bt_serialize.cpp
    sispopmq/connections.cpp
    sispopmq/jobs.cpp
    sispopmq/sispopmq.cpp
    sispopmq/proxy.cpp
    sispopmq/worker.cpp
)
set_target_properties(sispopmq PROPERTIES SOVERSION ${SISPOPMQ_LIBVERSION})

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(sispopmq PRIVATE Threads::Threads)

# libzmq is nearly impossible to link statically from a system-installed static library: it depends
# on a ton of other libraries, some of which are not all statically available.  If the caller wants
# to mess with this, so be it: they can set up a libzmq target and we'll use it.  Otherwise if they
# asked us to do things statically, don't even try to find a system lib and just build it.
set(sispopmq_build_static_libzmq OFF)
if(TARGET libzmq)
    target_link_libraries(sispopmq PUBLIC libzmq)
elseif(BUILD_SHARED_LIBS)
    include(FindPkgConfig)
    pkg_check_modules(libzmq libzmq>=4.3 IMPORTED_TARGET)

    if(libzmq_FOUND)
        # Debian sid includes a -isystem in the mit-krb package that, starting with pkg-config 0.29.2,
        # breaks cmake's pkgconfig module because it stupidly thinks "-isystem" is a path, so if we find
        # -isystem in the include dirs then hack it out.
        get_property(zmq_inc TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
        list(FIND zmq_inc "-isystem" broken_isystem)
        if(NOT broken_isystem EQUAL -1)
          list(REMOVE_AT zmq_inc ${broken_isystem})
          set_property(TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${zmq_inc})
        endif()

        target_link_libraries(sispopmq PUBLIC PkgConfig::libzmq)
    else()
        set(sispopmq_build_static_libzmq ON)
    endif()
else()
    set(sispopmq_build_static_libzmq ON)
endif()

if(sispopmq_build_static_libzmq)
    message(STATUS "libzmq >= 4.3 not found or static build requested, building bundled version")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
    target_link_libraries(sispopmq PUBLIC libzmq_vendor)
endif()

target_include_directories(sispopmq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cppzmq>
)

target_compile_options(sispopmq PRIVATE -Wall -Wextra -Werror)
set_target_properties(sispopmq PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

function(link_dep_libs target linktype libdirs)
    foreach(lib ${ARGN})
        find_library(link_lib-${lib} NAMES ${lib} PATHS ${libdirs})
        if(link_lib-${lib})
            target_link_libraries(${target} ${linktype} ${link_lib-${lib}})
        endif()
    endforeach()
endfunction()

# If the caller has already set up a sodium target then we will just link to it, otherwise we go
# looking for it.
if(TARGET sodium)
    target_link_libraries(sispopmq PUBLIC sodium)
    if(sispopmq_build_static_libzmq)
        target_link_libraries(libzmq_vendor INTERFACE sodium)
    endif()
else()
    include(FindPkgConfig)
    pkg_check_modules(sodium REQUIRED libsodium IMPORTED_TARGET)

    if(BUILD_SHARED_LIBS)
        target_link_libraries(sispopmq PUBLIC PkgConfig::sodium)
        if(sispopmq_build_static_libzmq)
            target_link_libraries(libzmq_vendor INTERFACE PkgConfig::sodium)
        endif()
    else()
        link_dep_libs(sispopmq PUBLIC "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
        target_include_directories(sispopmq PUBLIC ${sodium_STATIC_INCLUDE_DIRS})
        if(sispopmq_build_static_libzmq)
            link_dep_libs(libzmq_vendor INTERFACE "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
            target_link_libraries(libzmq_vendor INTERFACE ${sodium_STATIC_INCLUDE_DIRS})
        endif()
    endif()
endif()

add_library(sispopmq::sispopmq ALIAS sispopmq)
if(SISPOPMQ_LOKIMQ_COMPAT)
    add_library(lokimq::lokimq ALIAS sispopmq)
endif()

export(
    TARGETS sispopmq
    NAMESPACE sispopmq::
    FILE sispopmqTargets.cmake
)

if(SISPOPMQ_INSTALL)
    install(
        TARGETS sispopmq
        EXPORT sispopmqConfig
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )

    install(
        FILES sispopmq/address.h
        sispopmq/auth.h
        sispopmq/base32z.h
        sispopmq/base64.h
        sispopmq/batch.h
        sispopmq/bt_serialize.h
        sispopmq/bt_value.h
        sispopmq/byte_type.h
        sispopmq/connections.h
        sispopmq/hex.h
        sispopmq/sispopmq.h
        sispopmq/message.h
        sispopmq/variant.h
        ${CMAKE_CURRENT_BINARY_DIR}/sispopmq/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sispopmq
        )

    if(SISPOPMQ_INSTALL_CPPZMQ)
        install(
            FILES cppzmq/zmq.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sispopmq
            )
    endif()


    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/libsispopmq.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    if(SISPOPMQ_LOKIMQ_COMPAT)
        install(
            FILES lokimq/address.h
            lokimq/auth.h
            lokimq/base32z.h
            lokimq/base64.h
            lokimq/batch.h
            lokimq/bt_serialize.h
            lokimq/bt_value.h
            lokimq/connections.h
            lokimq/hex.h
            lokimq/lokimq.h
            lokimq/message.h
            lokimq/variant.h
            lokimq/version.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lokimq
            )

        install(
            FILES ${CMAKE_CURRENT_BINARY_DIR}/liblokimq.pc
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
            )
    endif()


endif()

if(SISPOPMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
