cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(FF_SRC_G726
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_g726.c
)

SET(FF_LIB_G726
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libavutil.a
)

SET(FF_INCLUDE_G726
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(ffmpeg-g726
        "${FF_SRC_G726}"
        "${FF_LIB_G726}"
        []
        GPAC_HAS_FFMPEG
        "${FF_INCLUDE_G726}"
        ""
        "1")
