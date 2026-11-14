/*
** File:        deflate.h
** Description: Deflate (RFC 1951) compressor and decompressor and CRC-32,
**              for the ZIP and gzip formats. Written for arcedit from the
**              RFC, no outside code.
**
** Change history:
**  2026-11-14 orgin, created
*/

#ifndef DEFLATE_H
#define DEFLATE_H

#include <stdint.h>

/* CRC-32 (polynomial 0xEDB88320, as used by ZIP and gzip); start with 0 */
uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len);

/* Compress len bytes of in as a raw deflate stream into out (outmax bytes).
   Returns the compressed size, or 0 when it doesn't fit into outmax.
   deflate_bound() gives an outmax that always fits (stored blocks). */
uint32_t deflate_compress(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t outmax);
uint32_t deflate_bound(uint32_t len);

/* Decompress a raw deflate stream. *out is malloc'ed (free it), *outlen
   gets its size, *used the number of input bytes the stream took.
   Returns 0 on success, -1 for bad data, -2 when out of memory. */
int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t **out, uint32_t *outlen, uint32_t *used);

#endif
