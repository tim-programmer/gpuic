# FriBidi has no CMake build of its own (meson/autotools only), so this builds
# the library from a release tarball, which ships the generated *.tab.i tables.
# Defines the static target gpuic_fribidi with the alias FriBidi::FriBidi.
function(gpuic_add_fribidi src)
  if(TARGET FriBidi::FriBidi)
    return()
  endif()
  file(READ "${src}/meson.build" _meson)
  string(REGEX MATCH "version: *'([0-9]+)\\.([0-9]+)\\.([0-9]+)'" _ "${_meson}")
  set(FRIBIDI_MAJOR_VERSION ${CMAKE_MATCH_1})
  set(FRIBIDI_MINOR_VERSION ${CMAKE_MATCH_2})
  set(FRIBIDI_MICRO_VERSION ${CMAKE_MATCH_3})
  set(FRIBIDI_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
  file(STRINGS "${src}/meson.build" _iface REGEX "^interface_version *= *[0-9]+")
  string(REGEX MATCH "[0-9]+" FRIBIDI_INTERFACE_VERSION "${_iface}")
  set(PACKAGE fribidi)
  set(PACKAGE_NAME "GNU FriBidi")
  set(PACKAGE_BUGREPORT "https://github.com/fribidi/fribidi/issues/new")
  set(SIZEOF_INT ${CMAKE_SIZEOF_INT})
  if(NOT SIZEOF_INT)
    set(SIZEOF_INT 4)
  endif()
  if(MSVC)
    set(FRIBIDI_MSVC_BUILD_PLACEHOLDER "#define FRIBIDI_BUILT_WITH_MSVC")
  else()
    set(FRIBIDI_MSVC_BUILD_PLACEHOLDER "#undef FRIBIDI_BUILT_WITH_MSVC")
  endif()

  set(gen "${CMAKE_CURRENT_BINARY_DIR}/gpuic_fribidi_gen")
  configure_file("${src}/lib/fribidi-config.h.in" "${gen}/fribidi-config.h" @ONLY)
  file(WRITE "${gen}/config.h" "#define HAVE_STDLIB_H 1\n#define HAVE_STRING_H 1\n#define HAVE_STRINGIZE 1\n")

  set(names fribidi fribidi-arabic fribidi-bidi fribidi-bidi-types fribidi-char-sets
    fribidi-char-sets-cap-rtl fribidi-char-sets-cp1255 fribidi-char-sets-cp1256
    fribidi-char-sets-iso8859-6 fribidi-char-sets-iso8859-8 fribidi-char-sets-utf8
    fribidi-deprecated fribidi-joining fribidi-joining-types fribidi-mirroring
    fribidi-brackets fribidi-run fribidi-shape)
  set(sources "")
  foreach(n IN LISTS names)
    list(APPEND sources "${src}/lib/${n}.c")
  endforeach()

  add_library(gpuic_fribidi STATIC ${sources})
  add_library(FriBidi::FriBidi ALIAS gpuic_fribidi)
  target_include_directories(gpuic_fribidi PRIVATE "${src}/lib" "${gen}")
  # fribidi.h includes fribidi-config.h by its bare name, so both are on the public path.
  target_include_directories(gpuic_fribidi SYSTEM PUBLIC "${src}/lib" "${gen}")
  target_compile_definitions(gpuic_fribidi PRIVATE HAVE_CONFIG_H FRIBIDI_BUILD PUBLIC FRIBIDI_LIB_STATIC)
  set_target_properties(gpuic_fribidi PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  if(NOT MSVC)
    target_compile_options(gpuic_fribidi PRIVATE -w)
  endif()
endfunction()
