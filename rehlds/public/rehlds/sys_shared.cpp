/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/
#include "sys_shared.h"

// CPUID is x86-only; <cpuid.h> does not exist on other architectures.
#if defined(__i386__) || defined(__x86_64__)
	#define REHLDS_HAVE_CPUID 1
#endif

#if defined(__GNUC__) && defined(REHLDS_HAVE_CPUID)
#include <cpuid.h>
#elif _MSC_VER >= 1400 && !(defined(ASMLIB_H) && defined(HAVE_OPT_STRTOOLS))
#include <intrin.h>	// __cpuidex
#endif

#define SSE3_FLAG		(1<<0)
#define SSSE3_FLAG		(1<<9)
#define SSE4_1_FLAG		(1<<19)
#define SSE4_2_FLAG		(1<<20)
#define POPCNT_FLAG		(1<<23)
#define AVX_FLAG		(1<<28)
#define AVX2_FLAG		(1<<5)

cpuinfo_t cpuinfo;

// asmlib.h is included unconditionally by precompiled.h, so ASMLIB_H is defined even in
// builds that do not LINK asmlib -- for example any 64-bit build, since lib/linux32/
// libaelf32.a is a 32-bit archive. Gating on ASMLIB_H alone therefore called cpuid_ex()
// from a library that was not linked, which links fine (undefined symbols are permitted in
// a shared object) and then fails at dlopen with "undefined symbol: cpuid_ex".
// strtools.h:67 already requires both symbols; match it.
void Sys_CheckCpuInstructionsSupport(void)
{
#ifndef REHLDS_HAVE_CPUID
	// Non-x86: every flag here names an x86 SIMD extension, so they are all absent by
	// definition. The consumers already dispatch on these at runtime and fall back to the
	// scalar implementations in mathlib.cpp / crc32c.cpp, so zeroing is the correct answer
	// rather than a stub. An AArch64 build wanting NEON should add its own feature bits.
	Q_memset(&cpuinfo, 0, sizeof(cpuinfo));
#else
	unsigned int cpuid_data[4];

#if defined(ASMLIB_H) && defined(HAVE_OPT_STRTOOLS)
	cpuid_ex((int *)cpuid_data, 1, 0);
#elif defined(__GNUC__)
	__get_cpuid(0x1, &cpuid_data[0], &cpuid_data[1], &cpuid_data[2], &cpuid_data[3]);
#else
	__cpuidex((int *)cpuid_data, 1, 0);
#endif

	cpuinfo.sse3 = (cpuid_data[2] & SSE3_FLAG) ? 1 : 0; // ecx
	cpuinfo.ssse3 = (cpuid_data[2] & SSSE3_FLAG) ? 1 : 0;
	cpuinfo.sse4_1 = (cpuid_data[2] & SSE4_1_FLAG) ? 1 : 0;
	cpuinfo.sse4_2 = (cpuid_data[2] & SSE4_2_FLAG) ? 1 : 0;
	cpuinfo.popcnt = (cpuid_data[2] & POPCNT_FLAG) ? 1 : 0;
	cpuinfo.avx = (cpuid_data[2] & AVX_FLAG) ? 1 : 0;

#if defined(ASMLIB_H) && defined(HAVE_OPT_STRTOOLS)
	cpuid_ex((int *)cpuid_data, 7, 0);
#elif defined(__GNUC__)
	__get_cpuid(0x7, &cpuid_data[0], &cpuid_data[1], &cpuid_data[2], &cpuid_data[3]);
#else
	__cpuidex((int *)cpuid_data, 7, 0);
#endif

	cpuinfo.avx2 = (cpuid_data[1] & AVX2_FLAG) ? 1 : 0; // ebx
#endif // REHLDS_HAVE_CPUID
}
