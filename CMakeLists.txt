cmake_minimum_required(VERSION 3.20)
project(gpuic VERSION 0.1.0 LANGUAGES C)

# The C++ layer (include/gpuic/gpuic.hpp) is header only; building its example and test needs a C++20 compiler.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  set(GPUIC_HAVE_CXX ON)
endif()

# Examples, tests and install rules default to on only when gpuic is the top
# level project, so add_subdirectory()/FetchContent users get just the library.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
  set(_gpuic_top ${PROJECT_IS_TOP_LEVEL})
elseif(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_gpuic_top ON)
else()
  set(_gpuic_top OFF)
endif()
option(GPUIC_BUILD_EXAMPLES "Build the example programs" ${_gpuic_top})
option(GPUIC_BUILD_TESTS "Build and register the tests" ${_gpuic_top})
option(GPUIC_INSTALL "Generate install rules" ${_gpuic_top})
option(GPUIC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer; any undefined behavior ends the program (GCC or clang)" OFF)
if(GPUIC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
set(_gpuic_test_env "GPUIC_FONT_CACHE=${CMAKE_CURRENT_BINARY_DIR}/font-scan-cache;GPUIC_LCD=off")
if(GPUIC_SANITIZE)   # (leaks off: the GL driver leaves a few bytes in every windowed test; the C++ tests link the sanitizer runtime late)
  list(APPEND _gpuic_test_env "ASAN_OPTIONS=detect_leaks=0:verify_asan_link_order=0" "UBSAN_OPTIONS=print_stacktrace=1")
endif()

if(_gpuic_top)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS ON)   # (gnu++20: the C macros use compound literals)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(GpuicDependencies)
find_package(Threads REQUIRED)

add_library(gpuic
  src/anim.c
  src/gfx.c
  src/image.c
  src/svg.c
  src/svgx.c
  src/curve.c
  src/app.c
  src/arena.c
  src/coro.c
  src/element.c
  src/exec.c
  src/input.c
  src/layout.c
  src/observe.c
  src/paint.c
  src/platform_sdl.c
  src/render_gl.c
  src/scroll.c
  src/text_input.c
  src/text.c
  src/views.c
  src/vlist.c)
add_library(gpuic::gpuic ALIAS gpuic)
set_target_properties(gpuic PROPERTIES
  C_STANDARD 11             # builder macros rely on __VA_OPT__ (gcc/clang)
  C_STANDARD_REQUIRED ON
  VERSION ${PROJECT_VERSION})
target_include_directories(gpuic
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src)
target_compile_options(gpuic PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_features(gpuic PUBLIC c_std_11)   # the public builder macros use __VA_OPT__
# The public header includes only the C library, so the dependencies stay private.
# An installed gpuic finds the same dependencies again in its gpuicConfig.cmake.
target_link_libraries(gpuic
  PRIVATE $<BUILD_INTERFACE:gpuic_deps> $<INSTALL_INTERFACE:gpuic_installed_deps>
  PUBLIC Threads::Threads)
if(NOT WIN32)
  target_link_libraries(gpuic PUBLIC m)
endif()
if(GPUIC_USES_FONTCONFIG)
  target_compile_definitions(gpuic PRIVATE GPUIC_HAVE_FONTCONFIG)
endif()

# The C++ layer: link gpuic::cpp for C++20 and the header.
add_library(gpuic_cpp INTERFACE)
add_library(gpuic::cpp ALIAS gpuic_cpp)
target_link_libraries(gpuic_cpp INTERFACE gpuic)
target_compile_features(gpuic_cpp INTERFACE cxx_std_20)
set_target_properties(gpuic_cpp PROPERTIES EXPORT_NAME cpp)

if(GPUIC_BUILD_EXAMPLES)
  foreach(example counter form scroll list async inputs bidi observe animate fd fonts gradients images svg)
    add_executable(${example} examples/${example}.c)
    target_link_libraries(${example} PRIVATE gpuic)
    target_compile_options(${example} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endforeach()
  if(GPUIC_HAVE_CXX)
    add_executable(counter_cpp examples/counter_cpp.cpp)
    target_link_libraries(counter_cpp PRIVATE gpuic::cpp)
    target_compile_options(counter_cpp PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endif()

if(GPUIC_BUILD_TESTS)
  enable_testing()
  # exec_test, fd_test, anim_test, window_test, autohide_test, fonts_test, lcd_test, input_test, list_test, observe_test and views_test open a window and need a display; the others do not.
  add_executable(cables_bench tests/cables_bench.c)   # not a test: a measurement (needs a display)
  target_link_libraries(cables_bench PRIVATE gpuic gpuic_deps)
  foreach(test exec_test fd_test anim_test window_test autohide_test fonts_test lcd_test gfx_test gradient_test image_test svg_test svgx_test box_test flex_test minwidth_test reveal_test track_test smooth_test bigdoc_test grapheme_test variations_test gamma_test touch_test sticky_test hlist_test hvlist_test border_style_test state_style_test paintshift_test transform_test curve_test stroke_test xform2_test lstate_test layer_test effects_test shapes_test xform3d_test textscale_test svgrelease_test pointer_test emptyview_test wheel_test shutdown_test keys_test input_test ime_wrap_test dualcaret_test wrapedit_test lazywrap_test longline_test text_test list_test vlist_test observe_test views_test)
    add_executable(${test} tests/${test}.c)
    target_link_libraries(${test} PRIVATE gpuic gpuic_deps)   # tests reach into internals and the dependency headers
    target_include_directories(${test} PRIVATE src)
    if(GPUIC_USES_FONTCONFIG)
      target_compile_definitions(${test} PRIVATE GPUIC_HAVE_FONTCONFIG)
    endif()
    target_compile_options(${test} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME ${test} COMMAND ${test})
    # tests never touch the user's real font scan cache
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "${_gpuic_test_env}")
  endforeach()
  if(GPUIC_HAVE_CXX)
    # the C++ layer: the same interface built with the C macros and with the chain must come out identical (needs a display),
    # and the generated chain methods must be what the C header says now (does not)
    add_executable(cpp_test tests/cpp_test.cpp tests/cpp_probe.c)
    target_link_libraries(cpp_test PRIVATE gpuic::cpp gpuic_deps)
    target_include_directories(cpp_test PRIVATE src)
    if(GPUIC_USES_FONTCONFIG)
      target_compile_definitions(cpp_test PRIVATE GPUIC_HAVE_FONTCONFIG)
    endif()
    target_compile_options(cpp_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
    add_test(NAME cpp_test COMMAND cpp_test)
    set_tests_properties(cpp_test PROPERTIES ENVIRONMENT "${_gpuic_test_env}")
    # the header and the C++ layer as ISO C++ (no compound literals): -Wpedantic -Werror
    add_executable(cpp_pedantic tests/cpp_pedantic.cpp)
    target_link_libraries(cpp_pedantic PRIVATE gpuic::cpp gpuic_deps)
    target_compile_options(cpp_pedantic PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>)
    add_test(NAME cpp_pedantic COMMAND cpp_pedantic)
    # timers, background work, coroutines, observation, text fields and loaders: every closure destroyed exactly once
    add_executable(cpp_async_test tests/cpp_async_test.cpp)
    target_link_libraries(cpp_async_test PRIVATE gpuic::cpp gpuic_deps)
    target_compile_options(cpp_async_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
    add_test(NAME cpp_async_test COMMAND cpp_async_test)
    set_tests_properties(cpp_async_test PROPERTIES ENVIRONMENT "${_gpuic_test_env}")
    # scroll and focus handles, key bindings, actions, the clipboard
    add_executable(cpp_input_test tests/cpp_input_test.cpp tests/cpp_probe.c)
    target_link_libraries(cpp_input_test PRIVATE gpuic::cpp gpuic_deps)
    target_include_directories(cpp_input_test PRIVATE src)
    if(GPUIC_USES_FONTCONFIG)
      target_compile_definitions(cpp_input_test PRIVATE GPUIC_HAVE_FONTCONFIG)
    endif()
    target_compile_options(cpp_input_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
    add_test(NAME cpp_input_test COMMAND cpp_input_test)
    set_tests_properties(cpp_input_test PROPERTIES ENVIRONMENT "${_gpuic_test_env}")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
      add_test(NAME cpp_chain_fresh COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_cpp_chain.py --check)
    endif()
  endif()
  # the grapheme tables and test strings use the Python `regex` module's Unicode properties: check them when it is there
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import regex" RESULT_VARIABLE _regex_missing OUTPUT_QUIET ERROR_QUIET)
    if(NOT _regex_missing)
      add_test(NAME grapheme_data_fresh COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_grapheme.py --check)
    endif()
  endif()
endif()

if(GPUIC_INSTALL)
  if(GPUIC_FETCHED)
    # Downloaded dependencies are static libraries inside this build tree, so an
    # installed libgpuic could not name them. Install against system packages instead.
    message(STATUS "gpuic: install rules skipped (downloaded dependencies: ${GPUIC_FETCHED}); "
                   "use GPUIC_DEPENDENCIES=SYSTEM for an installable build")
  else()
    include(CMakePackageConfigHelpers)
    set(GPUIC_CONFIG_FONTCONFIG OFF)
    if(GPUIC_USES_FONTCONFIG)
      set(GPUIC_CONFIG_FONTCONFIG ON)
    endif()
    install(TARGETS gpuic gpuic_cpp EXPORT gpuicTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT gpuicTargets NAMESPACE gpuic:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gpuic)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/gpuicConfigVersion.cmake
      VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    configure_package_config_file(cmake/gpuicConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/gpuicConfig.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gpuic)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gpuicConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/gpuicConfigVersion.cmake
      cmake/GpuicDependencies.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gpuic)
  endif()
endif()
