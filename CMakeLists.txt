cmake_minimum_required(VERSION 3.20)

project(WuwaUID LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Directory holding reshade.hpp and friends (the ReShade "include" folder).
set(RESHADE_INCLUDE_DIR "" CACHE PATH "Path to the ReShade include directory")

if(NOT EXISTS "${RESHADE_INCLUDE_DIR}/reshade.hpp")
	message(FATAL_ERROR
		"RESHADE_INCLUDE_DIR is not set or does not contain reshade.hpp.\n"
		"Pass -DRESHADE_INCLUDE_DIR=<path to reshade/include>.")
endif()

add_library(WuwaUID MODULE src/main.cpp)

target_include_directories(WuwaUID PRIVATE "${RESHADE_INCLUDE_DIR}")

target_compile_definitions(WuwaUID PRIVATE
	RESHADE_ADDON=1
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	UNICODE
	_UNICODE)

if(MSVC)
	target_compile_options(WuwaUID PRIVATE /W4 /EHsc /utf-8 /permissive-)
	# Static CRT keeps the add-on self-contained next to ReShade.
	set_property(TARGET WuwaUID PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# ReShade loads 64-bit add-ons from files ending in .addon64.
set_target_properties(WuwaUID PROPERTIES
	PREFIX ""
	SUFFIX ".addon64"
	OUTPUT_NAME "WuwaUID")

message(STATUS "WuwaUID: ReShade headers at ${RESHADE_INCLUDE_DIR}")
