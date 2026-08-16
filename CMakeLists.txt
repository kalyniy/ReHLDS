cmake_minimum_required(VERSION 3.1)
project(rehlds CXX)

if (WIN32)
	message(FATAL_ERROR "CMakeLists.txt Windows platform isn't supported yet. Use msvc/ReHLDS.sln instead it!")
endif()

# EXPERIMENTAL. 64 is portability reconnaissance only and does not produce a working
# server yet -- see docs/audit/15-x86-64-recon.md and 16-steam-64bit.md.
set(REHLDS_TARGET_BITS "32" CACHE STRING "Target word size: 32 (supported) or 64 (experimental)")
set_property(CACHE REHLDS_TARGET_BITS PROPERTY STRINGS 32 64)

# Architecture is decided here so every subproject agrees. A CMake toolchain file
# (cmake/toolchain-aarch64.cmake) selects AArch64, where no -m flag applies at all;
# otherwise REHLDS_TARGET_BITS picks the x86 word size.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
	set(REHLDS_ARCH_X86 FALSE)
	set(ARCH_FLAG "")
elseif (REHLDS_TARGET_BITS STREQUAL "64")
	set(REHLDS_ARCH_X86 TRUE)
	set(ARCH_FLAG "-m64")
else()
	set(REHLDS_ARCH_X86 TRUE)
	set(ARCH_FLAG "-m32")
endif()

add_custom_target(appversion DEPENDS
	COMMAND "${PROJECT_SOURCE_DIR}/rehlds/version/appversion.sh" "${PROJECT_SOURCE_DIR}"
)

add_subdirectory(rehlds)

if (NOT CMAKE_BUILD_TYPE MATCHES Unittests)
	add_subdirectory(rehlds/dedicated)
	add_subdirectory(rehlds/filesystem)

	# HLTV has not been ported; it still hardcodes -m32/-msse3 and would fight the 64-bit deps.
	if (REHLDS_ARCH_X86 AND REHLDS_TARGET_BITS STREQUAL "32")
		add_subdirectory(rehlds/HLTV)
	endif()
endif()
