#----------------------------------------
# 1. Preparing build:
#    rm -rf build
#    mkdir build && cd build
#
# 2. Select compiler and build it
# - Compile with Clang:
#    CC="clang" CXX="clang++" cmake ..
#    make
#
# - Compile with Intel C++ Compiler:
#    CC="icc" CXX="icpc" cmake ..
#    make
#
# - Compile with GCC Compiler:
#    cmake ..
#    make
#----------------------------------------

cmake_minimum_required(VERSION 3.1)
project(engine CXX)

option(DEBUG "Build with debug information." OFF)
option(USE_STATIC_LIBSTDC "Enables static linking libstdc++." OFF)

# The vendored lib/linux32/{librt,libm}.so are old-glibc import stubs, kept so that
# release binaries bind to ancient symbol versions and run on legacy server distros
# (see rehlds/version/glibc_test.sh). The vendored librt.so imports __libc_dlopen_mode,
# __libc_dlsym and __pthread_unwind from GLIBC_PRIVATE; those were removed in glibc 2.34
# when librt/libpthread/libdl were folded into libc, so linking it fails outright on any
# host with glibc >= 2.34 (Ubuntu 22.04+, Debian 12+).
#
# Turning this OFF links rt/m from the host toolchain by exact soname instead, which
# builds on a modern host at the cost of raising the binary's minimum glibc. Use it for
# local development, benchmarking and sanitizer builds; leave it ON for anything shipped.
option(USE_VENDORED_COMPAT_LIBS "Link the bundled old-glibc librt/libm import stubs (required for legacy-distro release builds; breaks on glibc >= 2.34)." ON)

# Both of these have complete, already-tested portable fallbacks in the tree, but until now
# there was no way to select them: REHLDS_JIT and REHLDS_SSE were defined unconditionally.
#
#   REHLDS_ENABLE_JIT OFF -> delta.cpp takes its non-JIT C++ path throughout, and all of
#     delta_jit.cpp compiles away. The x86 delta JIT is an IA-32 code generator (fixed
#     esi/edi/ebx/ebp allocation, 4-byte stack slots, 32-bit cdecl calls), so it cannot
#     survive an x86-64 or ARM64 port as-is; the brief's plan is to disable it for first
#     bring-up on those targets.
#
#   REHLDS_ENABLE_SSE OFF -> mathlib.cpp's scalar AngleVectors/BoxOnPlaneSide/etc. replace
#     mathlib_sse.cpp. Note the SSE paths already dispatch at runtime on cpuinfo, and
#     mathlib_tests.cpp force cpuinfo.sse4_1 = 0 to exercise the scalar twin, so both sides
#     are covered by the existing suite.
#
# Defaults ON preserve upstream behaviour exactly. Turning them off on x86-32 first is what
# lets the portability work change one variable at a time, rather than debugging the
# fallbacks and a new architecture simultaneously.
option(REHLDS_ENABLE_JIT "Build the x86 delta JIT. OFF selects the portable C++ delta path." ON)
option(REHLDS_ENABLE_SSE "Build the SSE mathlib paths. OFF selects the scalar implementations." ON)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Avoid -rdynamic -fPIC options
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

# EXPERIMENTAL, for portability reconnaissance only. 64 does not produce a working server
# and is not expected to yet -- see docs/audit/15-x86-64-recon.md. It exists so the port's
# task list can be derived from real compiler output instead of from source reading.
set(REHLDS_TARGET_BITS "32" CACHE STRING "Target word size: 32 (supported) or 64 (experimental, does not work)")
set_property(CACHE REHLDS_TARGET_BITS PROPERTY STRINGS 32 64)

if (REHLDS_TARGET_BITS STREQUAL "64")
	set(ARCH_FLAG "-m64")
	# A .so without -fPIC merely works by accident on i386; on x86-64 it fails to link with
	# R_X86_64_32 relocations against .text, so PIC has to come back on.
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	set(ENGINE_PIC ON)
	# engine_i486 is an i386 name. Follow ReGameDLL/Xash3D's convention instead so the two
	# architectures can coexist in one install (common/port.h:112-116 already defines
	# SO_ARCH_SUFFIX "_amd64.so" for this and has never been used).
	set(ENGINE_OUTPUT_NAME engine_amd64)
else()
	set(ARCH_FLAG "-m32")
	set(ENGINE_PIC OFF)
	set(ENGINE_OUTPUT_NAME engine_i486)
endif()

set(COMPILE_FLAGS "${ARCH_FLAG} -U_FORTIFY_SOURCE -DFORTIFY_SOURCE=0")
set(LINK_FLAGS "${ARCH_FLAG}")

set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wall -fno-rtti -fno-exceptions -fno-strict-aliasing -fno-strict-overflow")

# Remove noxref code and data
set(COMPILE_FLAGS "${COMPILE_FLAGS} -ffunction-sections -fdata-sections")

if (DEBUG)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g3 -O3 -ggdb")
else()
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g0 -O3 -fno-stack-protector")
endif()

# Check Intel C++ compiler
if ("$ENV{CXX}" MATCHES "icpc")
	# -fp-model=precise
	# ICC uses -fp-model fast=1 by default for more aggressive optimizations on floating-point calculations
	# https://software.intel.com/content/www/us/en/develop/documentation/cpp-compiler-developer-guide-and-reference/top/compiler-reference/compiler-options/compiler-option-details/floating-point-options/fp-model-fp.html#fp-model-fp_GUID-99936BBA-1508-4E9F-AC09-FA98613CE2F5
	set(COMPILE_FLAGS "${COMPILE_FLAGS} \
		-fp-model=precise\
		-fasm-blocks\
		-Qoption,cpp,--treat_func_as_string_literal_cpp")

	set(LINK_FLAGS "${LINK_FLAGS} -static-intel -no-intel-extensions")

	if (NOT DEBUG)
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -ipo")
		set(LINK_FLAGS "${LINK_FLAGS} -ipo")
	endif()
else()
	# Produce code optimized for the most common IA32/AMD64/EM64T processors.
	# As new processors are deployed in the marketplace, the behavior of this option will change.
	set(COMPILE_FLAGS "${COMPILE_FLAGS} \
		-mtune=generic -msse3\
		-fpermissive -fno-sized-deallocation\
		-Wno-unknown-pragmas -Wno-invalid-offsetof\
		-Wno-unused-variable -Wno-unused-result -Wno-unused-function -Wno-delete-non-virtual-dtor\
		-Wno-write-strings -Wno-format\
		-Wno-sign-compare -Wno-strict-aliasing -Wno-ignored-attributes")

	# Check if not Clang compiler
	if (NOT "$ENV{CXX}" MATCHES "clang")
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -fno-plt -Wno-unused-but-set-variable")

		# GCC >= 8.3
		if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
			set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wno-stringop-truncation -Wno-format-truncation -Wno-class-memaccess")
		endif()
	endif()
endif()

# GCC >= 8.3
if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -fcf-protection=none")
endif()

if (NOT DEBUG)
	set(LINK_FLAGS "${LINK_FLAGS} \
		-Wl,-gc-sections -Wl,--version-script=\"${PROJECT_SOURCE_DIR}/../version_script.lds\"")
endif()

set(PROJECT_SRC_DIR
	"${PROJECT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/engine"
	"${PROJECT_SOURCE_DIR}/common"
	"${PROJECT_SOURCE_DIR}/pm_shared"
	"${PROJECT_SOURCE_DIR}/rehlds"
	"${PROJECT_SOURCE_DIR}/testsuite"
	"${PROJECT_SOURCE_DIR}/unittests"
)

set(PROJECT_BZIP2_DIR
	"${PROJECT_SOURCE_DIR}/../dep/bzip2/include"
)

set(PROJECT_CPPUNITLITE_DIR
	"${PROJECT_SOURCE_DIR}/../dep/cppunitlite/include"
)

set(PROJECT_PUBLIC_DIR
	"${PROJECT_SOURCE_DIR}/public"
	"${PROJECT_SOURCE_DIR}/public/rehlds"
)

set(ENGINE_SRCS
	engine/sv_main.cpp
	engine/sv_user.cpp
	engine/sv_phys.cpp
	engine/sv_move.cpp
	engine/sv_pmove.cpp
	engine/sv_log.cpp
	engine/sv_remoteaccess.cpp
	engine/sv_steam3.cpp
	engine/sv_upld.cpp
	engine/sys_dll.cpp
	engine/sys_dll2.cpp
	engine/sys_engine.cpp
	engine/sys_linuxwind.cpp
	engine/SystemWrapper.cpp
	engine/host.cpp
	engine/host_cmd.cpp
	engine/net_chan.cpp
	engine/net_ws.cpp
	engine/pmove.cpp
	engine/pmovetst.cpp
	engine/pr_cmds.cpp
	engine/pr_edict.cpp
	engine/wad.cpp
	engine/model.cpp
	engine/world.cpp
	engine/zone.cpp
	engine/cmd.cpp
	engine/cmodel.cpp
	engine/com_custom.cpp
	engine/common.cpp
	engine/crc.cpp
	engine/cvar.cpp
	engine/decals.cpp
	engine/delta.cpp
	engine/delta_jit.cpp
	engine/ed_strpool.cpp
	engine/filesystem.cpp
	engine/filesystem_internal.cpp
	engine/hashpak.cpp
	engine/info.cpp
	engine/ipratelimit.cpp
	engine/l_studio.cpp
	engine/textures.cpp
	engine/tmessage.cpp
	engine/traceinit.cpp
	engine/unicode_strtools.cpp
	engine/buildnum.cpp
	engine/mathlib.cpp
	engine/mathlib_sse.cpp
	engine/md5.cpp
	engine/mem.cpp
	engine/module.cpp
	engine/r_studio.cpp
	engine/vid_null.cpp
	engine/cl_null.cpp
	engine/snd_null.cpp
	engine/sse_mathfun.cpp
	engine/public_amalgamation.cpp
	$<$<STREQUAL:${REHLDS_TARGET_BITS},64>:${PROJECT_SOURCE_DIR}/engine/steam_stub_64.cpp>
	rehlds/flight_recorder.cpp
	rehlds/FlightRecorderImpl.cpp
	rehlds/frameperf.cpp
	rehlds/hitreg_stats.cpp
	rehlds/hookchains_impl.cpp
	rehlds/main.cpp
	rehlds/platform.cpp
	rehlds/public_amalgamation.cpp
	rehlds/rehlds_api_impl.cpp
	rehlds/rehlds_interfaces_impl.cpp
	rehlds/rehlds_messagemngr_impl.cpp
	rehlds/rehlds_security.cpp
)

set(UNITTESTS_SRCS
	unittests/common_tests.cpp
	unittests/crc32c_tests.cpp
	unittests/delta_tests.cpp
	unittests/info_tests.cpp
	unittests/mathlib_tests.cpp
	unittests/rehlds_tests_shared.cpp
	unittests/rehlds_tests_shared.h
	unittests/security_tests.cpp
	unittests/struct_offsets_tests.cpp
	unittests/TestRunner.cpp
	unittests/tmessage_tests.cpp
	unittests/unicode_tests.cpp
)

set(COMMON_SRCS
	"common/BaseSystemModule.cpp"
	"common/ObjectList.cpp"
	"common/TokenLine.cpp"
)

set(PUBLIC_SRCS
	"public/tier0/dbg.cpp"
	"public/registry.cpp"
	"public/steamid.cpp"
	"public/utlbuffer.cpp"
)

if (CMAKE_BUILD_TYPE MATCHES Unittests)
	if (NOT TARGET cppunitlite)
		add_subdirectory(../dep/cppunitlite cppunitlite)
	endif()

	add_executable(engine ${appversion.sh} ${ENGINE_SRCS})
	target_link_libraries(engine PRIVATE cppunitlite)
else()
	add_library(engine SHARED ${appversion.sh} ${ENGINE_SRCS})
endif()

if (NOT TARGET bzip2)
	add_subdirectory(../dep/bzip2 bzip2)
endif()

if (NOT TARGET appversion)
	add_custom_target(appversion DEPENDS COMMAND "${PROJECT_SOURCE_DIR}/version/appversion.sh" "${PROJECT_SOURCE_DIR}/..")
endif()

add_dependencies(engine appversion)

target_include_directories(engine PRIVATE
	${PROJECT_SRC_DIR}
	${PROJECT_BZIP2_DIR}
	${PROJECT_CPPUNITLITE_DIR}
	${PROJECT_PUBLIC_DIR}
)

target_compile_definitions(engine PRIVATE
	SWDS
	$<$<BOOL:${REHLDS_ENABLE_JIT}>:REHLDS_JIT>
	$<$<BOOL:${REHLDS_ENABLE_SSE}>:REHLDS_SSE>
	REHLDS_FIXES
	REHLDS_CHECKS
	REHLDS_API
	REHLDS_SELF
	REHLDS_OPT_PEDANTIC
	# asmlib (lib/linux32/libaelf32.a) is a 32-bit-only static library with no source in
	# tree. Without HAVE_OPT_STRTOOLS, public/strtools.h:108+ selects a complete libc
	# fallback for Q_memcpy/Q_strlen/Q_stricmp and friends -- correct everywhere, at some
	# cost on the hottest string paths.
	$<$<STREQUAL:${REHLDS_TARGET_BITS},32>:HAVE_OPT_STRTOOLS>
	USE_BREAKPAD_HANDLER
	_LINUX
	LINUX
	_GLIBCXX_USE_CXX11_ABI=0
	_stricmp=strcasecmp
	_strnicmp=strncasecmp
	_strdup=strdup
	_unlink=unlink
	_vsnprintf=vsnprintf
	_vsnwprintf=vswprintf

	$<$<CONFIG:Unittests>:REHLDS_UNIT_TESTS>
)

target_sources(engine PRIVATE
	${COMMON_SRCS}
	${PUBLIC_SRCS}

	$<$<CONFIG:Unittests>:
	${UNITTESTS_SRCS}>
)

if (USE_VENDORED_COMPAT_LIBS)
	set(COMPAT_RT_LIB rt)
	set(COMPAT_M_LIB m)
else()
	# -l:<soname> bypasses the vendored lib/linux32/lib{rt,m}.so link stubs, which would
	# otherwise win because that -L path precedes the system ones. aelf32 and steam_api
	# still resolve from lib/linux32 as normal.
	set(COMPAT_RT_LIB -l:librt.so.1)
	set(COMPAT_M_LIB -l:libm.so.6)
endif()

if (REHLDS_TARGET_BITS STREQUAL "32")
	set(ARCH_ONLY_LIBS aelf32)
	set(STEAM_LIB steam_api)
else()
	# aelf32 is a 32-bit static archive; see the HAVE_OPT_STRTOOLS note above.
	set(ARCH_ONLY_LIBS "")
	# The bundled libsteam_api.so is 32-bit, and the modern 64-bit one implements
	# SteamGameServer015 against these SteamGameServer011 headers. engine/steam_stub_64.cpp
	# supplies the symbols instead. See docs/audit/16-steam-64bit.md.
	set(STEAM_LIB "")
endif()

target_link_libraries(engine PRIVATE
	dl
	${COMPAT_RT_LIB}
	${COMPAT_M_LIB}
	${ARCH_ONLY_LIBS}
	bzip2
	${STEAM_LIB}
)

if (USE_STATIC_LIBSTDC)
	target_compile_definitions(engine PRIVATE BUILD_STATIC_LIBSTDC)
	set(LINK_FLAGS "${LINK_FLAGS} -static-libgcc -static-libstdc++")
endif()

set(LINK_FLAGS "${LINK_FLAGS} \
	-Wl,-rpath,'$ORIGIN/.' \
	-L${PROJECT_SOURCE_DIR}/lib/linux32")

set_target_properties(engine PROPERTIES
	OUTPUT_NAME ${ENGINE_OUTPUT_NAME}
	PREFIX ""
	COMPILE_FLAGS ${COMPILE_FLAGS}
	LINK_FLAGS ${LINK_FLAGS}
	# Non-PIC is an i386-only luxury; x86-64 shared objects require it.
	POSITION_INDEPENDENT_CODE ${ENGINE_PIC}
)
