cmake_minimum_required(VERSION 3.20)
project(lsfg-cap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# OFF builds only the dependency-free core + tests (used by CI, which has
# no SDL3/PipeWire/portal packages).
option(LSFG_BUILD_APP "Build the lsfg-cap executable" ON)
include(CTest)

# Pure logic shared by the app and the unit tests; no system dependencies.
add_library(lsfg_core STATIC
    src/core/cadence.cpp
    src/core/pacer.cpp
)
target_include_directories(lsfg_core PUBLIC src)
target_compile_options(lsfg_core PRIVATE -Wall -Wextra)

if(LSFG_BUILD_APP)
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

    target_compile_options(lsfg-cap PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_libraries(lsfg-cap PRIVATE
        lsfg_core
        Vulkan::Vulkan
        PkgConfig::PIPEWIRE
        PkgConfig::PORTAL
        PkgConfig::SDL3
    )

    install(TARGETS lsfg-cap RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(lsfg_test_cadence tests/test_cadence.cpp)
    target_compile_options(lsfg_test_cadence PRIVATE -Wall -Wextra)
    target_link_libraries(lsfg_test_cadence PRIVATE lsfg_core)
    add_test(NAME cadence COMMAND lsfg_test_cadence)

    add_executable(lsfg_test_pacer tests/test_pacer.cpp)
    target_compile_options(lsfg_test_pacer PRIVATE -Wall -Wextra)
    target_link_libraries(lsfg_test_pacer PRIVATE lsfg_core)
    add_test(NAME pacer COMMAND lsfg_test_pacer)
endif()
