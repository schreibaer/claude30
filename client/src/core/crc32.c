#include "crc32.h"

static unsigned long table[256];
static int built;

static void build(void)
{
    unsigned long c;
    int i, k;
    for (i = 0; i < 256; i++) {
        c = (unsigned long)i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    built = 1;
}

unsigned long crc32_add(unsigned long crc, const char *data, long n)
{
    long i;
    if (!built)
        build();
    for (i = 0; i < n; i++)
        crc = table[(crc ^ (unsigned char)data[i]) & 0xFF] ^ ((crc >> 8) & 0x00FFFFFFUL);
    return crc & 0xFFFFFFFFUL;
}

unsigned long crc32_buf(const char *data, long n)
{
    return crc32_end(crc32_add(CRC32_INIT, data, n));
}
