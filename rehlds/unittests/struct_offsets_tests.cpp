#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#pragma warning(push)
#ifndef _WIN32
#pragma warning(disable : 1875)		// warning #1875: offsetof applied to non-POD (Plain Old Data) types is nonstandard
#endif // _WIN32

#define CHECK_STRUCT_SIZE(s,win_size,lin_size) {\
	int needOff = __isWindows ? win_size : lin_size; \
	UINT32_EQUALS("Bad size "#s"::", needOff, sizeof(s)); \
}

#define CHECK_STRUCT_OFFSET(s,f,win_off,lin_off) {\
	int needOff = __isWindows ? win_off : lin_off; \
	int realOff = offsetof(s, f); \
	UINT32_EQUALS("Bad offset "#s"::"#f, needOff, realOff); \
	}

// These offsets pin client_t to the layout of Valve's original 32-bit engine so that
// third-party binaries (Metamod, AMXX, plugins) which hard-code offsets keep working. They
// describe an IN-MEMORY ABI, not a serialized format, and client_t embeds netchan_t (four
// pointers), client_frame_t*, edict_t*, two resource_t and more -- so every one of them
// legitimately changes on any 64-bit target. Checking them there tests nothing real and
// fails by construction: on AArch64, chokecount lands at 9600 rather than 9264.
//
// The plugin-compatibility guarantee they encode only exists on i386 anyway, since a 64-bit
// engine cannot load 32-bit plugins at all.
#if defined(__i386__) || defined(_M_IX86)
TEST(StructOffsets, ReversingChecks, 5000)
{
	CHECK_STRUCT_OFFSET(client_t, active, 0, 0);
	CHECK_STRUCT_OFFSET(client_t, chokecount, 0x2540, 0x2430);
	CHECK_STRUCT_OFFSET(client_t, datagram, 0x25C0, 0x24AC);
	CHECK_STRUCT_OFFSET(client_t, m_VoiceStreams, 0x5000, 0x4EE0);
	CHECK_STRUCT_OFFSET(client_t, m_lastvoicetime, 0x5008, 0x4EE8);
	CHECK_STRUCT_OFFSET(client_t, datagram_buf, 0x25D4, 0x24C0);
	CHECK_STRUCT_OFFSET(client_t, connection_started, 0x3578, 0x3460);

	//CHECK_STRUCT_SIZE(server_t, 0x46418, 0x4640C);

	printf("sizeof server_t: 0x%2X\n", sizeof(server_t));
	printf("sizeof CSteam3Server: 0x%2X\n", sizeof(CSteam3Server));
	//printf("offsetof CSteam3Server::m_SteamIDGS: 0x%2X\n", offsetof(CSteam3Server, m_SteamIDGS));

	//CHECK_STRUCT_OFFSET(CSteam3, m_bLogOnResult, 5, 5);
	//CHECK_STRUCT_OFFSET(CSteam3Server, m_bLogOnResult, 5, 5);

	//CHECK_STRUCT_OFFSET(CSteam3Server, m_bLanOnly, 0x86, 0x9E);
	//CHECK_STRUCT_OFFSET(CSteam3Server, m_SteamIDGS, 0x87, 0x9F);
}
#endif // i386

#pragma warning( pop )
