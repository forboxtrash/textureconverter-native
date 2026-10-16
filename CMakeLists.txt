# Конвертер текстур - Texture Converter, v3.4, автор Antoñŷ
# Сборка tc_native.so (расширение Ruby) под Windows x64, MSVC, статическая
# линковка кодеков (/MD, триплет vcpkg x64-windows-static-md).
# В версии 3.4 включены WEBP и AVIF (чтение и запись). RAW и HEIF выключены.
# НЕ ПРОВЕРЕНО в реальной сборке под Windows: имена целей CMake (WebP::webp, avif)
# смотрите в файлах usage портов vcpkg и поправьте при необходимости.
cmake_minimum_required(VERSION 3.20)
project(tc_native C)

# SketchUp собирает Ruby с динамическим CRT (/MD), расширение должно совпадать.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Заголовки и библиотека Ruby 3.2 из репозитория SketchUp/ruby-c-extension-examples:
#   SU_RUBY_INCLUDE = <репозиторий>\ThirdParty\include\ruby\3.2\win32_x64
#                     (в ней лежат и ruby.h, и ruby\config.h)
#   SU_RUBY_LIB     = <репозиторий>\ThirdParty\lib\win32\x64-ucrt-ruby320.lib
set(SU_RUBY_INCLUDE        "" CACHE PATH     "Папка с ruby.h (ThirdParty/include/ruby/3.2/win32_x64)")
set(SU_RUBY_CONFIG_INCLUDE "" CACHE PATH     "Дополнительная папка с ruby/config.h (для Ruby 3.2 SketchUp не нужна)")
set(SU_RUBY_LIB            "" CACHE FILEPATH "Файл x64-ucrt-ruby320.lib (ThirdParty/lib/win32)")

option(TC_WITH_WEBP "WEBP: чтение и запись (libwebp, BSD-3)"                       ON)
option(TC_WITH_AVIF "AVIF: чтение (dav1d) и запись (aom) через libavif, BSD-2"      ON)
option(TC_WITH_RAW  "RAW: чтение (LibRaw, CDDL-1.0 или LGPL-2.1), в 3.4 не нужно"  OFF)
option(TC_WITH_HEIF "HEIC/HEIF: чтение (libheif, LGPL-3, патенты HEVC), в 3.4 не нужно" OFF)

foreach(v SU_RUBY_INCLUDE SU_RUBY_LIB)
  if(NOT ${v})
    message(FATAL_ERROR "Укажите -D${v}=... (см. README)")
  endif()
endforeach()

add_library(tc_native MODULE tc_native.c)
set_target_properties(tc_native PROPERTIES PREFIX "" SUFFIX ".so")
target_include_directories(tc_native PRIVATE ${SU_RUBY_INCLUDE})
if(SU_RUBY_CONFIG_INCLUDE)
  target_include_directories(tc_native PRIVATE ${SU_RUBY_CONFIG_INCLUDE})
endif()
target_link_libraries(tc_native PRIVATE ${SU_RUBY_LIB})
target_compile_options(tc_native PRIVATE /utf-8 /O2 /W3)

if(TC_WITH_WEBP)
  find_package(WebP CONFIG REQUIRED)
  target_compile_definitions(tc_native PRIVATE TC_WITH_WEBP)
  # WebP::webp содержит и декодер, и энкодер; WebP::webpdecoder не добавляем,
  # чтобы не получить дубли символов при статической линковке.
  target_link_libraries(tc_native PRIVATE WebP::webp)
endif()
if(TC_WITH_AVIF)
  find_package(libavif CONFIG REQUIRED)
  target_compile_definitions(tc_native PRIVATE TC_WITH_AVIF)
  target_link_libraries(tc_native PRIVATE avif)
endif()
if(TC_WITH_RAW)
  find_package(LibRaw CONFIG REQUIRED)
  target_compile_definitions(tc_native PRIVATE TC_WITH_RAW)
  target_link_libraries(tc_native PRIVATE libraw::raw)
endif()
if(TC_WITH_HEIF)
  find_package(libheif CONFIG REQUIRED)
  target_compile_definitions(tc_native PRIVATE TC_WITH_HEIF)
  target_link_libraries(tc_native PRIVATE heif)
endif()
