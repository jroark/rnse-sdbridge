/* The SD bus's two checksums: CRC7 of commands and responses, CRC16
 * (CCITT, polynomial 0x1021, from 0) of a data line's block. */
#ifndef CRC_H
#define CRC_H
#include <stdint.h>

void crc_init(void);
/* The 7 bits of CRC7 over n bytes. */
uint8_t crc7(const uint8_t *p, int n);
/* CRC16 over the bytes of n 32-bit words, most significant byte first. */
uint16_t crc16_words(const uint32_t *w, int n);

#endif
