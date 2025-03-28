
cmake_minimum_required(VERSION 3.10)
project(phototest2)

# 헤더 검색 경로
include_directories(${CMAKE_SOURCE_DIR}/include)
#include_directories("/Users/meo/photoTest2/Complete_arm64-v8a/include")

# 네이티브 라이브러리
add_library(native-lib SHARED native-lib.cpp)

# JNI libs 경로
set(JNI_LIB_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})

# Core shared libs
foreach(lib gphoto2 gphoto2_port ltdl usb)
    add_library(${lib} SHARED IMPORTED)
    set_target_properties(${lib} PROPERTIES
            IMPORTED_LOCATION "${JNI_LIB_DIR}/lib${lib}.so"
    )
endforeach()

# Port plugins — 파일명 그대로 사용
add_library(gphoto2_port_iolib_usb1 SHARED IMPORTED)
set_target_properties(gphoto2_port_iolib_usb1 PROPERTIES
        IMPORTED_LOCATION "${JNI_LIB_DIR}/libgphoto2_port_iolib_usb1.so"
)

add_library(gphoto2_port_iolib_disk SHARED IMPORTED)
set_target_properties(gphoto2_port_iolib_disk PROPERTIES
        IMPORTED_LOCATION "${JNI_LIB_DIR}/libgphoto2_port_iolib_disk.so"
)

## Nikon PTP2 driver
#add_library(camdriver SHARED IMPORTED)
#set_target_properties(camdriver PROPERTIES
#        IMPORTED_LOCATION "${JNI_LIB_DIR}/libptp2.so"
#)

# Android 시스템 라이브러리
find_library(log-lib log)
find_library(android-lib android)

# 링크
target_link_libraries(native-lib
        gphoto2_port
        gphoto2
        ltdl
        usb
        gphoto2_port_iolib_usb1
        gphoto2_port_iolib_disk
#        camdriver
        ${log-lib}
        ${android-lib}
)



#cmake_minimum_required(VERSION 3.10)
#project(phototest2)
#
## 헤더 디렉토리
#include_directories(${CMAKE_SOURCE_DIR}/include)
#include_directories("/Users/meo/photoTest2/Complete_arm64-v8a/include")
#
## JNI 라이브러리 디렉토리
#set(JNI_LIB_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})
#
## .so 파일 수집
#file(GLOB GPHOTO_SO_FILES "${JNI_LIB_DIR}/*.so")
#
## 링크할 목록 및 로그 헤더 생성 준비
#set(LINKED_LIBS "")
#set(SO_LOG_LINES "")
#set(ADDED_LIBS "") # 이미 추가된 라이브러리 이름 기록용
#
#foreach(sofile ${GPHOTO_SO_FILES})
#    get_filename_component(libname ${sofile} NAME_WE)
#    get_filename_component(fullname ${sofile} NAME)
#
#    # 중복 방지: 이미 추가된 경우 건너뛰기
#    list(FIND ADDED_LIBS ${libname} lib_index)
#    if(lib_index EQUAL -1)
#        add_library(${libname} SHARED IMPORTED)
#        set_target_properties(${libname} PROPERTIES IMPORTED_LOCATION ${sofile})
#        list(APPEND LINKED_LIBS ${libname})
#        list(APPEND ADDED_LIBS ${libname})
#        set(SO_LOG_LINES "${SO_LOG_LINES}LOGD(\"Loaded SO: ${fullname}\");\n")
#    endif()
#endforeach()
#
## 자동 로그 헤더 파일 생성
#file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
#file(WRITE ${CMAKE_BINARY_DIR}/generated/so_list.h
#        "#pragma once\n"
#        "#include <android/log.h>\n"
#        "#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, \"SO-LOADER\", __VA_ARGS__)\n\n"
#        "static void logLoadedSOs() {\n"
#        "${SO_LOG_LINES}" "}\n")
#include_directories(${CMAKE_BINARY_DIR}/generated)
#
## 네이티브 라이브러리
#add_library(native-lib SHARED native-lib.cpp)
#
## Android 시스템 라이브러리
#find_library(log-lib log)
#find_library(android-lib android)
#
## 최종 링크
#target_link_libraries(native-lib
#        ${LINKED_LIBS}
#        ${log-lib}
#        ${android-lib})
