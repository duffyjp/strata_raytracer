cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(RayTracer LANGUAGES C CXX)

# One self-contained executable: static CRT, no third-party DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC on Windows (see build.ps1).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ------------------------------------------------------------------ shaders -> embedded SPIR-V
find_program(RT_GLSLANG NAMES glslang glslangValidator
             PATHS "${RT_ROOT}/tools/glslang/bin" REQUIRED)
find_program(RT_PYTHON NAMES python python3 py REQUIRED)

file(GLOB RT_SHADER_SRC CONFIGURE_DEPENDS
     "${RT_ROOT}/shaders/*.glsl" "${RT_ROOT}/shaders/*.rgen" "${RT_ROOT}/shaders/*.rchit"
     "${RT_ROOT}/shaders/*.rmiss" "${RT_ROOT}/shaders/*.rcall" "${RT_ROOT}/shaders/*.comp"
     "${RT_ROOT}/shaders/*.vert" "${RT_ROOT}/shaders/*.frag")

set(RT_SPV_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders_spv.h")
add_custom_command(
  OUTPUT ${RT_SPV_HEADER}
  COMMAND ${RT_PYTHON} "${RT_ROOT}/tools/build_shaders.py"
          --shaders "${RT_ROOT}/shaders"
          --glslang "${RT_GLSLANG}"
          --out-dir "${CMAKE_CURRENT_BINARY_DIR}/generated/glsl"
          --header "${RT_SPV_HEADER}"
  DEPENDS ${RT_SHADER_SRC} "${RT_ROOT}/tools/build_shaders.py"
  COMMENT "compiling GLSL -> SPIR-V and embedding"
  VERBATIM)
add_custom_target(rt_shaders DEPENDS ${RT_SPV_HEADER})

# ------------------------------------------------------------------ vulkathird (loader + headers)
add_library(volk STATIC "${RT_ROOT}/third_party/volk/volk.c")
target_include_directories(volk PUBLIC
  "${RT_ROOT}/third_party/volk"
  "${RT_ROOT}/third_party/vulkan_headers")
target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)
target_compile_definitions(volk PRIVATE VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)
if(MSVC)
  # volk.c is C89-ish; silence the usual CRT-name complaints for a self-contained build.
  target_compile_options(volk PRIVATE /W3 /wd4996)
endif()

# ------------------------------------------------------------------ GLFW (static, same CRT)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
set(GLFW_USE_HYBRID_HPG ON CACHE BOOL "" FORCE)   # prefer the discrete GPU on hybrid laptops
add_subdirectory("${RT_ROOT}/third_party/glfw" EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------ the application
add_executable(raytracer WIN32
  "${RT_ROOT}/src/main.cpp"
  "${RT_ROOT}/src/core.cpp"
  "${RT_ROOT}/src/core2.cpp"
  "${RT_ROOT}/src/frame.cpp"
  "${RT_ROOT}/src/scene.cpp"
  "${RT_ROOT}/src/pak.cpp"
  "${RT_ROOT}/src/json.cpp"
  "${RT_SPV_HEADER}")

add_dependencies(raytracer rt_shaders)

# The generated header is #included by core2.cpp. Declaring it only as a target source lets Ninja
# compile that file in parallel with the shader step (observed: the exe linked against stale
# SPIR-V), so make it a real input of that object's rule.
set_source_files_properties("${RT_ROOT}/src/core2.cpp" PROPERTIES
  OBJECT_DEPENDS "${RT_SPV_HEADER}")

# A hand-generated copy of the header in the source tree would silently shadow the freshly built
# one (it did, once), so refuse to configure until it is gone.
if(EXISTS "${RT_ROOT}/src/generated/shaders_spv.h")
  message(FATAL_ERROR
    "src/generated/shaders_spv.h is a stale copy; delete it - the build generates "
    "${RT_SPV_HEADER}")
endif()

target_include_directories(raytracer PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"          # for #include "generated/shaders_spv.h" (generated)
  "${RT_ROOT}/src"
  "${RT_ROOT}/third_party/stb")

target_link_libraries(raytracer PRIVATE volk glfw shell32)
target_compile_definitions(raytracer PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(raytracer PRIVATE /W3 /wd4996 /wd4100 /wd4189 /wd4456 /wd4458 /O2 /Ob2)
  # Keep the exe free of any redist dependency beyond the static CRT.
  target_link_options(raytracer PRIVATE /INCREMENTAL:NO)
endif()

# Put the executable next to the pak so the runtime search finds it without extra copying.
set_target_properties(raytracer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${RT_ROOT}/build/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RT_ROOT}/build/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${RT_ROOT}/build/bin")
