#include <cstdint>
#include <cstring>
#include <bit>
#include <type_traits>

static inline uint64_t bswap64(uint64_t x)
{
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) <<  8) |
           ((x & 0x000000FF00000000ULL) >>  8) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

static inline uint64_t hostToLE64(uint64_t x)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return bswap64(x);
#elif defined(_WIN32)
    // Windows는 리틀엔디안
    return x;
#else
    // 대부분 리틀엔디안
    return x;
#endif
}

static inline void WriteF64LE(uint8_t* dst, double v)
{
    static_assert(sizeof(double) == 8, "double must be 8 bytes");

    uint64_t u = 0;
    std::memcpy(&u, &v, sizeof(u));   // double 비트패턴 추출
    u = hostToLE64(u);                // 리틀엔디안으로 변환(필요한 플랫폼만)
    std::memcpy(dst, &u, sizeof(u));  // payload에 기록
}