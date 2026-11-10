#ifndef SXCHNGE_CRC32_H
#define SXCHNGE_CRC32_H

#include <stdint.h>

// CRC-32 with the Koopman polynomial (crc32.Koopman in Go)
uint32_t sxCrc32(const unsigned char *message, uint32_t size);

#endif
