// Isolate the cost of the vec3 load strategy in mathlib_sse.cpp.
//
// The whole-server A/B cannot resolve this: bot AI dominates the frame tail and its
// within-condition variance is larger than any effect two instructions could have. This
// measures only the thing that changed.
//
// Build 32-bit to match the shipping target:
//   g++ -m32 -O2 -msse3 -mtune=generic -o loadbench loadbench.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <pmmintrin.h>

typedef float vec_t;
typedef vec_t vec3_t[3];

static inline void xmm2vec(vec_t *v, const __m128 m) {
	_mm_storel_pi((__m64 *)v, m);
	_mm_store_ss(v + 2, _mm_shuffle_ps(m, m, 0x02));
}

// A: upstream -- one 16-byte load from a 12-byte vector.
static inline __m128 load_A(const vec_t *v) { return _mm_loadu_ps(v); }

// B: the fix -- exactly 12 bytes, lane 3 zeroed.
static inline __m128 load_B(const vec_t *v) {
	return _mm_movelh_ps(_mm_loadl_pi(_mm_setzero_ps(), (const __m64 *)v), _mm_load_ss(v + 2));
}

#define MAKE(SUF, LOAD)                                                                 \
static void VectorMA_##SUF(const vec_t *a, float s, const vec_t *m, vec_t *out) {        \
	xmm2vec(out, _mm_add_ps(_mm_mul_ps(_mm_set_ps1(s), LOAD(m)), LOAD(a)));              \
}                                                                                        \
static float DotProduct_##SUF(const vec_t *a, const vec_t *b) {                          \
	__m128 v = _mm_mul_ps(LOAD(a), LOAD(b));                                             \
	return _mm_cvtss_f32(_mm_add_ps(_mm_movehl_ps(v, v), _mm_hadd_ps(v, v)));            \
}                                                                                        \
static float Length_##SUF(const vec_t *v) {                                              \
	__m128 x = LOAD(v);                                                                  \
	__m128 m = _mm_mul_ps(x, x);                                                         \
	return _mm_cvtss_f32(_mm_sqrt_ps(_mm_add_ps(_mm_movehl_ps(m, m), _mm_hadd_ps(m, m))));\
}

MAKE(A, load_A)
MAKE(B, load_B)

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// Walk a large array so the loads miss cache the way the engine's do, rather than measuring
// one hot vector in L1 -- docs/audit/22 measured a 31.5% cache-miss rate for this workload.
enum { N = 1 << 16, ITERS = 400 };
// PAD=1 gives every vector a 16-byte stride, so a 16-byte load can never cross a cache line.
// If A's penalty is cache-line splits, it must disappear here -- that is the control.
#ifdef PAD
struct padvec { vec_t v[3]; vec_t pad; };
static struct padvec buf_[N] __attribute__((aligned(16)));
#define VEC(i) (buf_[i].v)
#else
static vec3_t buf_[N];
#define VEC(i) (buf_[i])
#endif

#define BENCH(SUF)                                                                       \
	do {                                                                                 \
		volatile float sink = 0.0f;                                                      \
		double t0 = now_ms();                                                            \
		for (int it = 0; it < ITERS; it++) {                                             \
			for (int i = 0; i + 1 < N; i++) {                                            \
				vec3_t o;                                                                \
				VectorMA_##SUF(VEC(i), 0.5f, VEC(i + 1), o);                             \
				sink += o[0] + DotProduct_##SUF(VEC(i), VEC(i + 1)) + Length_##SUF(o);    \
			}                                                                            \
		}                                                                                \
		double dt = now_ms() - t0;                                                        \
		printf("  %-6s %8.1f ms   %6.2f ns/triple  (sink %g)\n", #SUF, dt,                \
		       dt * 1e6 / ((double)ITERS * (N - 1)), (double)sink);                        \
	} while (0)

int main(int argc, char **argv) {
	for (int i = 0; i < N; i++) {
		VEC(i)[0] = (float)(i % 97) * 0.31f;
		VEC(i)[1] = (float)(i % 53) * 1.7f;
		VEC(i)[2] = (float)(i % 31) * 0.9f;
	}

	// Verify the two produce identical results before timing them.
	int mismatches = 0;
	for (int i = 0; i + 1 < N; i++) {
		vec3_t oa, ob;
		VectorMA_A(VEC(i), 0.5f, VEC(i + 1), oa);
		VectorMA_B(VEC(i), 0.5f, VEC(i + 1), ob);
		if (memcmp(oa, ob, sizeof(oa)) != 0) mismatches++;
		if (DotProduct_A(VEC(i), VEC(i + 1)) != DotProduct_B(VEC(i), VEC(i + 1))) mismatches++;
		if (Length_A(VEC(i)) != Length_B(VEC(i))) mismatches++;
	}
	printf("equivalence: %d mismatches over %d vectors\n", mismatches, N - 1);

	printf("interleaved runs (A = _mm_loadu_ps, B = vec2xmm):\n");
	if (argc > 1 && argv[1][0] == 'A') { BENCH(A); return 0; }
	if (argc > 1 && argv[1][0] == 'B') { BENCH(B); return 0; }
	for (int rep = 0; rep < 3; rep++) { BENCH(A); BENCH(B); }
	return 0;
}
