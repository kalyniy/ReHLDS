#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

// Cyclic interpolation for the historical-pose rewind (docs/audit/21, 23).
//
// Both of these interpolate quantities that wrap, and both are only wrong on the wrap. An
// end-to-end test cannot see the defect: the value stays in range and merely describes the
// wrong pose, so nothing crashes and no assertion fires -- it just registers hits against a
// body that is facing the wrong way, or mid-animation instead of at its start. Hence direct
// tests of the wrap itself.

TEST(LerpAngleWrapTest, Lerp, 1000) {
	// Interior case: no wrap involved, plain linear result.
	DOUBLES_EQUAL("interior midpoint", 15.0f, SV_LerpAngle(10.0f, 20.0f, 0.5f), 0.0001);

	// Endpoints are exact.
	DOUBLES_EQUAL("frac=0 returns from", 10.0f, SV_LerpAngle(10.0f, 20.0f, 0.0f), 0.0001);
	DOUBLES_EQUAL("frac=1 returns to", 20.0f, SV_LerpAngle(10.0f, 20.0f, 1.0f), 0.0001);

	// The reason this function exists: crossing +-180 must take the short way (20 degrees
	// through the seam), not the long way (340 degrees back across the circle).
	DOUBLES_EQUAL("wrap +180 -> -180", 180.0f, SV_LerpAngle(170.0f, -170.0f, 0.5f), 0.0001);
	DOUBLES_EQUAL("wrap -180 -> +180", -180.0f, SV_LerpAngle(-170.0f, 170.0f, 0.5f), 0.0001);

	// Quarter of the way across the seam: 170 + 5 = 175.
	DOUBLES_EQUAL("wrap quarter", 175.0f, SV_LerpAngle(170.0f, -170.0f, 0.25f), 0.0001);

	// Result always lands back inside (-180, 180].
	for (int i = 0; i <= 10; i++) {
		float r = SV_LerpAngle(179.0f, -179.0f, i / 10.0f);
		CHECK("angle stays in range", r > -180.5f && r <= 180.5f);
	}
}

TEST(LerpFrameWrapTest, Lerp, 1000) {
	// Studio frames are cyclic on [0, 256) for looping sequences (ReGameDLL
	// animating.cpp:33-36), so the same short-path rule applies with a period of 256.

	DOUBLES_EQUAL("interior midpoint", 100.0f, SV_LerpFrame(50.0f, 150.0f, 0.5f), 0.0001);
	DOUBLES_EQUAL("frac=0 returns from", 50.0f, SV_LerpFrame(50.0f, 150.0f, 0.0f), 0.0001);
	DOUBLES_EQUAL("frac=1 returns to", 150.0f, SV_LerpFrame(50.0f, 150.0f, 1.0f), 0.0001);

	// The bug this replaced: from=250, to=6 is a 12-frame step forward through the wrap.
	// A plain from + (to - from) * frac gives 128 -- the middle of the animation, a
	// completely unrelated pose. Correct answer is 250 + 6 = 256 -> 0.
	DOUBLES_EQUAL("wrap 250 -> 6 midpoint", 0.0f, SV_LerpFrame(250.0f, 6.0f, 0.5f), 0.0001);

	// Quarter of the way: 250 + 3 = 253, still before the seam.
	DOUBLES_EQUAL("wrap quarter stays before seam", 253.0f, SV_LerpFrame(250.0f, 6.0f, 0.25f), 0.0001);

	// Three quarters: 250 + 9 = 259 -> 3.
	DOUBLES_EQUAL("wrap three quarters", 3.0f, SV_LerpFrame(250.0f, 6.0f, 0.75f), 0.0001);

	// Backwards across the seam is equally short-path: 6 -> 250 is 12 frames backwards.
	DOUBLES_EQUAL("wrap backwards", 0.0f, SV_LerpFrame(6.0f, 250.0f, 0.5f), 0.0001);

	// A genuine half-cycle step is ambiguous by definition; assert only that the result stays
	// in range rather than pinning a direction.
	for (int i = 0; i <= 10; i++) {
		float r = SV_LerpFrame(0.0f, 128.0f, i / 10.0f);
		CHECK("frame stays in range", r >= 0.0f && r < 256.0f);
	}

	// Every combination must land inside [0, 256) -- the studio code indexes with this.
	for (int a = 0; a < 256; a += 17) {
		for (int b = 0; b < 256; b += 23) {
			for (int f = 0; f <= 4; f++) {
				float r = SV_LerpFrame((float)a, (float)b, f / 4.0f);
				CHECK("frame in [0,256)", r >= 0.0f && r < 256.0f);
			}
		}
	}
}
