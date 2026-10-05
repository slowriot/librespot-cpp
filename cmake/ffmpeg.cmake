include(ExternalProject)
include(ProcessorCount)
processorcount(ffmpeg_jobs)
if(ffmpeg_jobs EQUAL 0)
  set(ffmpeg_jobs 1)
endif()
set(LIBRESPOT_DEPENDENCY_JOBS "${ffmpeg_jobs}" CACHE STRING "Parallel jobs when building FFmpeg")
find_package(PkgConfig QUIET)
option(LIBRESPOT_USE_SYSTEM_FFMPEG "Use installed FFmpeg libraries instead of building the pinned source" OFF)

if(LIBRESPOT_USE_SYSTEM_FFMPEG)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat>=62 libavcodec>=62 libavutil>=60)
  add_library(librespot_ffmpeg INTERFACE)
  target_link_libraries(librespot_ffmpeg INTERFACE PkgConfig::FFMPEG)
else()
  fetchcontent_declare(ffmpeg
    URL https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n8.0.tar.gz
    URL_HASH SHA256=dd4030dbfdc34d9ff255a116bdd1caade42500ac2981efa27f8b151cc54c7b9e
  )
  fetchcontent_makeavailable(ffmpeg)
  find_program(ffmpeg_make NAMES gmake make REQUIRED)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    find_program(ffmpeg_nasm NAMES nasm REQUIRED)
  endif()
  set(ffmpeg_prefix "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
  file(MAKE_DIRECTORY "${ffmpeg_prefix}/include")
  externalproject_add(ffmpeg_build
    SOURCE_DIR "${ffmpeg_SOURCE_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${ffmpeg_SOURCE_DIR}/configure"
      "--prefix=${ffmpeg_prefix}"
      "--cc=${CMAKE_C_COMPILER}"
      --disable-everything
      --disable-autodetect
      --disable-programs
      --disable-doc
      --disable-network
      --disable-avdevice
      --disable-avfilter
      --disable-swscale
      --disable-swresample
      --disable-shared
      --enable-static
      --enable-pic
      --enable-avcodec
      --enable-avformat
      --enable-avutil
      --enable-decoder=vorbis,flac,mp3float,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le
      --enable-demuxer=ogg,flac,mp3,wav
      --enable-parser=flac,mpegaudio,vorbis
      --enable-protocol=file
    BUILD_COMMAND "${ffmpeg_make}" "-j${LIBRESPOT_DEPENDENCY_JOBS}"
    INSTALL_COMMAND "${ffmpeg_make}" install
    BUILD_BYPRODUCTS
      "${ffmpeg_prefix}/lib/libavformat.a"
      "${ffmpeg_prefix}/lib/libavcodec.a"
      "${ffmpeg_prefix}/lib/libavutil.a"
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
  )
  foreach(component IN ITEMS avformat avcodec avutil)
    add_library(ffmpeg_${component} STATIC IMPORTED)
    set_target_properties(ffmpeg_${component} PROPERTIES
      IMPORTED_LOCATION "${ffmpeg_prefix}/lib/lib${component}.a"
      INTERFACE_INCLUDE_DIRECTORIES "${ffmpeg_prefix}/include"
    )
    add_dependencies(ffmpeg_${component} ffmpeg_build)
  endforeach()
  add_library(librespot_ffmpeg INTERFACE)
  target_link_libraries(librespot_ffmpeg
    INTERFACE ffmpeg_avformat
    INTERFACE ffmpeg_avcodec
    INTERFACE ffmpeg_avutil
    INTERFACE Threads::Threads
    INTERFACE m
  )
endif()
