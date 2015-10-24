#ifndef _LIBFDT_ENV_H
#define _LIBFDT_ENV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* IAMROOT-12A:
 * ------------
 * cpu_to_fdt16(x), cpu_to_fdt32(x), cpu_to_fdt64(x) 함수.
 * 16비트, 32비트, 64비트 단위로 CPU가 리틀엔디안인 경우에
 * 매크로가 의미가 있다.
 *
 * 참고로 디바이스트리는 빅엔디안으로 기록되어 있음.
 * CPU는 리틀엔디안 또는 빅엔디안이든 상관없이 동작.
 */

#define EXTRACT_BYTE(n)	((unsigned long long)((uint8_t *)&x)[n])
static inline uint16_t fdt16_to_cpu(uint16_t x)
{
	return (EXTRACT_BYTE(0) << 8) | EXTRACT_BYTE(1);
}
#define cpu_to_fdt16(x) fdt16_to_cpu(x)

static inline uint32_t fdt32_to_cpu(uint32_t x)
{
	return (EXTRACT_BYTE(0) << 24) | (EXTRACT_BYTE(1) << 16) | (EXTRACT_BYTE(2) << 8) | EXTRACT_BYTE(3);
}
#define cpu_to_fdt32(x) fdt32_to_cpu(x)

static inline uint64_t fdt64_to_cpu(uint64_t x)
{
	return (EXTRACT_BYTE(0) << 56) | (EXTRACT_BYTE(1) << 48) | (EXTRACT_BYTE(2) << 40) | (EXTRACT_BYTE(3) << 32)
		| (EXTRACT_BYTE(4) << 24) | (EXTRACT_BYTE(5) << 16) | (EXTRACT_BYTE(6) << 8) | EXTRACT_BYTE(7);
}
#define cpu_to_fdt64(x) fdt64_to_cpu(x)
#undef EXTRACT_BYTE

#endif /* _LIBFDT_ENV_H */
