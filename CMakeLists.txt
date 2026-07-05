cmake_minimum_required(VERSION 3.20)
project(lsfg-cap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PORTAL REQUIRED IMPORTED_TARGET libportal)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

add_executable(lsfg-cap
    src/main.cpp
    src/portal.cpp
    src/capture.cpp
    src/renderer.cpp
    src/vk/context.cpp
    src/vk/frame_pool.cpp
    src/vk/dmabuf_import.cpp
)

target_include_directories(lsfg-cap PRIVATE src)
target_compile_options(lsfg-cap PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(lsfg-cap PRIVATE
    Vulkan::Vulkan
    PkgConfig::PIPEWIRE
    PkgConfig::PORTAL
    PkgConfig::SDL3
)

install(TARGETS lsfg-cap RUNTIME DESTINATION bin)
