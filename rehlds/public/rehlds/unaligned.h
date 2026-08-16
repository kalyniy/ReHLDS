/*
*    Unaligned load/store helpers.
*
*    The delta encoder and the bit reader both address multi-byte values at arbitrary byte
*    offsets -- a field offset inside an entity_state_t, or a bit position inside a packet --
*    and both did it by casting the byte pointer and dereferencing. That is undefined
*    behaviour regardless of the target: the C++ object model requires the pointer to be
*    suitably aligned, and UBSan reports every one of them (docs/audit/17, 23).
*
*    It has worked because x86 permits unaligned access in hardware and AArch64 Linux permits
*    it for normal memory. The exposure is not the CPU, it is the compiler: having formed a
*    uint32* the optimiser may legitimately assume 4-byte alignment and, for example, vectorise
*    a surrounding loop with aligned moves.
*
*    memcpy carries no alignment requirement and both GCC and Clang fold these to exactly the
*    same single instruction as the cast -- verified on the 32-bit target, where `punned` and
*    `copied` emit byte-identical assembly. So this removes the UB at zero cost, and is
*    correct on architectures that fault on unaligned access.
*/

#pragma once

#include <string.h>

template <typename T>
inline T LoadUnaligned(const void *p)
{
	T v;
	memcpy(&v, p, sizeof(T));
	return v;
}

template <typename T>
inline void StoreUnaligned(void *p, T v)
{
	memcpy(p, &v, sizeof(T));
}
