#define ROUNDS 85000000U

static __attribute__((noinline)) unsigned crunch(unsigned x)
{
    for (unsigned i = 0; i < ROUNDS; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x *= 0x85ebca6bU;
    }
    return x & 0x7fffffffU;
}
