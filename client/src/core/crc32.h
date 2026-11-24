/* CRC-32 as in zlib (polynomial 0xEDB88320): crc32("123456789") is
   0xCBF43926. The 1 KB table is built on first use. Portable C. */
#ifndef CRC32_H
#define CRC32_H

#define CRC32_INIT 0xFFFFFFFFUL

unsigned long crc32_add(unsigned long crc, const char *data, long n);
#define crc32_end(c) (((c) ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL)
unsigned long crc32_buf(const char *data, long n);

#endif
