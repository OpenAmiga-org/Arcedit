/*
** File:        deflate.c
** Description: Deflate (RFC 1951) compressor and decompressor, CRC-32.
**
**  Compressor: LZ77 over a 32 KB window (hash chains, one step lazy
**  matching), dynamic Huffman blocks; stored blocks when packing doesn't
**  help. Decompressor: stored, fixed and dynamic blocks, canonical Huffman
**  decoding by code length counts.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdlib.h>
#include <string.h>

#include "deflate.h"

static const uint16_t LenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                       8193, 12289, 16385, 24577 };
static const uint8_t DistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
/* order of the code length code lengths in a dynamic block header */
static const uint8_t ClOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/*
** CRC-32 as used by ZIP and gzip
*/
uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
 static uint32_t table[256];
 static int ready = 0;
 uint32_t i;
 if(!ready)
 {
  for(i = 0; i < 256; i++)
  {
   uint32_t c = i;
   int k;
   for(k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
   table[i] = c;
  }
  ready = 1;
 }
 crc = ~crc;
 for(i = 0; i < len; i++) crc = (crc >> 8) ^ table[(crc ^ buf[i]) & 0xFF];
 return ~crc;
}

/****************************************************************************/
/* compressor                                                               */
/****************************************************************************/

#define WSIZE     32768
#define MAXMATCH  258
#define MINMATCH  3
#define HBITS     15
#define HSIZE     (1 << HBITS)
#define MAXCHAIN  128
#define BLOCKMAX  16384

struct deflater
{
 uint8_t *out;
 uint32_t outpos, outmax;
 uint32_t bitbuf;
 int bitcount;
 int overflow;

 uint16_t lit[BLOCKMAX];      /* literal byte, or match length when dist != 0 */
 uint16_t dist[BLOCKMAX];     /* 0 for literals */
 uint32_t ntok;

 uint32_t head[HSIZE];
 uint32_t prev[WSIZE];
};

/*
** Send the n low bits of x, least significant first (deflate bit order)
*/
static void put(struct deflater *d, int n, uint32_t x)
{
 d->bitbuf |= (x & ((1u << n) - 1)) << d->bitcount;
 d->bitcount += n;
 while(d->bitcount >= 8)
 {
  if(d->outpos >= d->outmax)
  {
   d->overflow = 1;
   d->bitcount = 0;
   d->bitbuf = 0;
   return;
  }
  d->out[d->outpos++] = (uint8_t)d->bitbuf;
  d->bitbuf >>= 8;
  d->bitcount -= 8;
 }
}

/*
** Send a Huffman code (stored most significant bit first in the stream)
*/
static void putcode(struct deflater *d, int len, uint32_t code)
{
 uint32_t rev = 0;
 int i;
 for(i = 0; i < len; i++) rev = (rev << 1) | ((code >> i) & 1);
 put(d, len, rev);
}

/*
** Huffman code lengths (at most maxbits) for freq[0..n-1]. At least two
** symbols always get a code, so every code set is complete.
*/
static void make_lengths(uint32_t *freq, int n, uint8_t *len, int maxbits)
{
 uint32_t f[2 * 320];
 int parent[2 * 320], alive[2 * 320];
 int used = 0, i, scale = 0;

 for(i = 0; i < n; i++) if(freq[i]) used++;
 /* make sure there are two symbols */
 for(i = 0; used < 2 && i < n; i++)
 {
  if(freq[i] == 0)
  {
   freq[i] = 1;
   used++;
  }
 }

 for(;;)
 {
  int nodes = n, maxl = 0;
  for(i = 0; i < n; i++)
  {
   f[i] = freq[i] ? ((freq[i] >> scale) | 1) : 0;
   alive[i] = freq[i] != 0;
   parent[i] = -1;
   len[i] = 0;
  }
  for(;;)
  {
   int a = -1, b = -1;
   for(i = 0; i < nodes; i++)
   {
    if(!alive[i]) continue;
    if(a < 0 || f[i] < f[a])
    {
     b = a;
     a = i;
    }
    else if(b < 0 || f[i] < f[b])
    {
     b = i;
    }
   }
   if(b < 0) break;
   f[nodes] = f[a] + f[b];
   alive[nodes] = 1;
   parent[nodes] = -1;
   alive[a] = alive[b] = 0;
   parent[a] = parent[b] = nodes;
   nodes++;
  }
  for(i = 0; i < n; i++)
  {
   int l = 0, p;
   if(!freq[i]) continue;
   for(p = parent[i]; p >= 0; p = parent[p]) l++;
   len[i] = (uint8_t)l;
   if(l > maxl) maxl = l;
  }
  if(maxl <= maxbits) return;
  scale++;
 }
}

/*
** Canonical codes for code lengths
*/
static void make_codes(const uint8_t *len, int n, uint16_t *code)
{
 uint16_t count[17], next[17];
 int i;
 memset(count, 0, sizeof(count));
 for(i = 0; i < n; i++) count[len[i]]++;
 count[0] = 0;
 next[0] = 0;
 next[1] = 0;
 for(i = 1; i < 16; i++) next[i + 1] = (uint16_t)((next[i] + count[i]) << 1);
 for(i = 0; i < n; i++) if(len[i]) code[i] = next[len[i]]++;
}

static int len_symbol(uint32_t l)
{
 int i;
 for(i = 28; i > 0; i--) if(l >= LenBase[i]) return i;
 return 0;
}

static int dist_symbol(uint32_t dd)
{
 int i;
 for(i = 29; i > 0; i--) if(dd >= DistBase[i]) return i;
 return 0;
}

/*
** Send the collected tokens as one dynamic Huffman block
*/
static void send_block(struct deflater *d, int last)
{
 uint32_t lfreq[286], dfreq[30], cfreq[19];
 uint8_t llen[286], dlen[30], clen[19], all[286 + 30];
 uint16_t lcode[286], dcode[30], ccode[19];
 uint8_t rle[286 + 30], rlex[286 + 30];
 int nlit, ndist, ncl, nall, nrle = 0, i;
 uint32_t t;

 memset(lfreq, 0, sizeof(lfreq));
 memset(dfreq, 0, sizeof(dfreq));
 memset(cfreq, 0, sizeof(cfreq));
 for(t = 0; t < d->ntok; t++)
 {
  if(d->dist[t] == 0) lfreq[d->lit[t]]++;
  else
  {
   lfreq[257 + len_symbol(d->lit[t])]++;
   dfreq[dist_symbol(d->dist[t])]++;
  }
 }
 lfreq[256] = 1;
 make_lengths(lfreq, 286, llen, 15);
 make_lengths(dfreq, 30, dlen, 15);
 make_codes(llen, 286, lcode);
 make_codes(dlen, 30, dcode);
 for(nlit = 286; nlit > 257 && llen[nlit - 1] == 0; nlit--);
 for(ndist = 30; ndist > 1 && dlen[ndist - 1] == 0; ndist--);

 /* the code lengths, run length coded */
 nall = nlit + ndist;
 memcpy(all, llen, (size_t)nlit);
 memcpy(all + nlit, dlen, (size_t)ndist);
 for(i = 0; i < nall;)
 {
  int run = 1;
  while(i + run < nall && all[i + run] == all[i]) run++;
  if(all[i] == 0 && run >= 3)
  {
   if(run > 138) run = 138;
   if(run <= 10)
   {
    rle[nrle] = 17;
    rlex[nrle++] = (uint8_t)(run - 3);
   }
   else
   {
    rle[nrle] = 18;
    rlex[nrle++] = (uint8_t)(run - 11);
   }
   i += run;
  }
  else if(all[i] != 0 && run >= 4)
  {
   int rep = run - 1;
   rle[nrle] = all[i];
   rlex[nrle++] = 0;
   if(rep > 6) rep = 6;
   rle[nrle] = 16;
   rlex[nrle++] = (uint8_t)(rep - 3);
   i += 1 + rep;
  }
  else
  {
   rle[nrle] = all[i];
   rlex[nrle++] = 0;
   i++;
  }
 }
 for(i = 0; i < nrle; i++) cfreq[rle[i]]++;
 make_lengths(cfreq, 19, clen, 7);
 make_codes(clen, 19, ccode);
 for(ncl = 19; ncl > 4 && clen[ClOrder[ncl - 1]] == 0; ncl--);

 put(d, 1, last ? 1 : 0);
 put(d, 2, 2);
 put(d, 5, (uint32_t)(nlit - 257));
 put(d, 5, (uint32_t)(ndist - 1));
 put(d, 4, (uint32_t)(ncl - 4));
 for(i = 0; i < ncl; i++) put(d, 3, clen[ClOrder[i]]);
 for(i = 0; i < nrle; i++)
 {
  putcode(d, clen[rle[i]], ccode[rle[i]]);
  if(rle[i] == 16) put(d, 2, rlex[i]);
  else if(rle[i] == 17) put(d, 3, rlex[i]);
  else if(rle[i] == 18) put(d, 7, rlex[i]);
 }

 for(t = 0; t < d->ntok && !d->overflow; t++)
 {
  if(d->dist[t] == 0)
  {
   putcode(d, llen[d->lit[t]], lcode[d->lit[t]]);
  }
  else
  {
   int ls = len_symbol(d->lit[t]), ds = dist_symbol(d->dist[t]);
   putcode(d, llen[257 + ls], lcode[257 + ls]);
   if(LenExtra[ls]) put(d, LenExtra[ls], d->lit[t] - LenBase[ls]);
   putcode(d, dlen[ds], dcode[ds]);
   if(DistExtra[ds]) put(d, DistExtra[ds], d->dist[t] - DistBase[ds]);
  }
 }
 putcode(d, llen[256], lcode[256]);
 d->ntok = 0;
}

static uint32_t dhash(const uint8_t *p)
{
 return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) & (HSIZE - 1);
}

static void dinsert(struct deflater *d, const uint8_t *in, uint32_t pos, uint32_t len)
{
 if(pos + MINMATCH <= len)
 {
  uint32_t h = dhash(in + pos);
  d->prev[pos & (WSIZE - 1)] = d->head[h];
  d->head[h] = pos + 1;
 }
}

static uint32_t dmatch(struct deflater *d, const uint8_t *in, uint32_t pos, uint32_t len, uint32_t *dist)
{
 uint32_t best = 0, cand, chain = MAXCHAIN;
 uint32_t maxlen = len - pos > MAXMATCH ? MAXMATCH : len - pos;
 if(maxlen < MINMATCH) return 0;
 cand = d->head[dhash(in + pos)];
 while(cand && chain--)
 {
  uint32_t c = cand - 1, l = 0;
  if(c >= pos || pos - c > WSIZE) break;
  if(in[c + best] == in[pos + best])
  {
   while(l < maxlen && in[c + l] == in[pos + l]) l++;
   if(l > best)
   {
    best = l;
    *dist = pos - c;
    if(l == maxlen) break;
   }
  }
  cand = d->prev[c & (WSIZE - 1)];
 }
 return best >= MINMATCH ? best : 0;
}

static void token(struct deflater *d, uint32_t lit, uint32_t dist)
{
 d->lit[d->ntok] = (uint16_t)lit;
 d->dist[d->ntok] = (uint16_t)dist;
 if(++d->ntok == BLOCKMAX) send_block(d, 0);
}

uint32_t deflate_bound(uint32_t len)
{
 return len + 5 * (len / 65535 + 1) + 16;
}

/*
** Stored blocks: always possible when outmax >= deflate_bound(len)
*/
static uint32_t store(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t outmax)
{
 uint32_t pos = 0, o = 0;
 do
 {
  uint32_t n = len - pos > 65535 ? 65535 : len - pos;
  if(o + 5 + n > outmax) return 0;
  out[o++] = (uint8_t)(pos + n >= len ? 1 : 0);
  out[o++] = (uint8_t)n;
  out[o++] = (uint8_t)(n >> 8);
  out[o++] = (uint8_t)~n;
  out[o++] = (uint8_t)(~n >> 8);
  memcpy(out + o, in + pos, n);
  o += n;
  pos += n;
 }
 while(pos < len);
 return o;
}

uint32_t deflate_compress(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t outmax)
{
 struct deflater *d = calloc(1, sizeof(*d));
 uint32_t pos = 0, result = 0;

 if(d == NULL) return 0;
 d->out = out;
 d->outmax = outmax;
 while(pos < len && !d->overflow)
 {
  uint32_t dist = 0, l = dmatch(d, in, pos, len, &dist);
  if(l)
  {
   uint32_t d2 = 0, l2;
   dinsert(d, in, pos, len);
   l2 = (pos + 1 < len) ? dmatch(d, in, pos + 1, len, &d2) : 0;
   if(l2 > l)
   {
    token(d, in[pos], 0);
    pos++;
    l = l2;
    dist = d2;
    dinsert(d, in, pos, len);
   }
   token(d, l, dist);
   {
    uint32_t k;
    for(k = 1; k < l; k++) dinsert(d, in, pos + k, len);
   }
   pos += l;
  }
  else
  {
   dinsert(d, in, pos, len);
   token(d, in[pos], 0);
   pos++;
  }
 }
 if(!d->overflow)
 {
  send_block(d, 1);
  if(d->bitcount > 0) put(d, 8 - d->bitcount, 0);
 }
 if(!d->overflow && d->outpos < deflate_bound(len) - 16 && d->outpos <= len + 5 * (len / 65535 + 1))
 {
  result = d->outpos;
 }
 free(d);
 /* packing didn't help: stored blocks */
 if(result == 0 || result > len + 5 * (len / 65535 + 1)) result = store(in, len, out, outmax);
 return result;
}

/****************************************************************************/
/* decompressor                                                             */
/****************************************************************************/

struct huff
{
 uint16_t count[16];      /* codes of each length */
 uint16_t symbol[320];    /* symbols in canonical order */
};

struct inflater
{
 const uint8_t *in;
 uint32_t inlen, pos;
 uint32_t bitbuf;
 int bitcount;
 uint8_t *out;
 uint32_t outlen, outmax;
 uint32_t limit;          /* stop after this much output (0: no limit) */
 int error;               /* < 0 bad data / memory, 1 limit reached */
};

static uint32_t bits(struct inflater *s, int n)
{
 uint32_t v;
 while(s->bitcount < n)
 {
  if(s->pos >= s->inlen)
  {
   s->error = -1;
   return 0;
  }
  s->bitbuf |= (uint32_t)s->in[s->pos++] << s->bitcount;
  s->bitcount += 8;
 }
 v = s->bitbuf & ((1u << n) - 1);
 s->bitbuf >>= n;
 s->bitcount -= n;
 return v;
}

/*
** Build a decoding table from code lengths. Returns 0, or -1 when the
** lengths are over-subscribed.
*/
static int build(struct huff *h, const uint8_t *len, int n)
{
 uint16_t offs[16];
 int i, left = 1;
 memset(h->count, 0, sizeof(h->count));
 for(i = 0; i < n; i++) h->count[len[i]]++;
 for(i = 1; i < 16; i++)
 {
  left <<= 1;
  left -= h->count[i];
  if(left < 0) return -1;
 }
 offs[1] = 0;
 for(i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
 for(i = 0; i < n; i++) if(len[i]) h->symbol[offs[len[i]]++] = (uint16_t)i;
 return 0;
}

static int decode(struct inflater *s, const struct huff *h)
{
 int code = 0, first = 0, index = 0, l;
 for(l = 1; l < 16; l++)
 {
  int count;
  code |= (int)bits(s, 1);
  if(s->error) return -1;
  count = h->count[l];
  if(code - count < first) return h->symbol[index + (code - first)];
  index += count;
  first += count;
  first <<= 1;
  code <<= 1;
 }
 s->error = -1;
 return -1;
}

static int outbyte(struct inflater *s, uint8_t c)
{
 if(s->limit && s->outlen >= s->limit)
 {
  s->error = 1;
  return -1;
 }
 if(s->outlen == s->outmax)
 {
  uint32_t max = s->outmax ? s->outmax * 2 : 65536;
  uint8_t *p = realloc(s->out, max);
  if(p == NULL)
  {
   s->error = -2;
   return -1;
  }
  s->out = p;
  s->outmax = max;
 }
 s->out[s->outlen++] = c;
 return 0;
}

static int codes(struct inflater *s, const struct huff *lh, const struct huff *dh)
{
 for(;;)
 {
  int sym = decode(s, lh);
  if(s->error) return s->error;
  if(sym < 256)
  {
   if(outbyte(s, (uint8_t)sym)) return s->error;
  }
  else if(sym == 256)
  {
   return 0;
  }
  else
  {
   uint32_t len, dist;
   sym -= 257;
   if(sym >= 29) return -1;
   len = LenBase[sym] + bits(s, LenExtra[sym]);
   sym = decode(s, dh);
   if(s->error || sym < 0 || sym >= 30) return -1;
   dist = DistBase[sym] + bits(s, DistExtra[sym]);
   if(s->error || dist > s->outlen) return -1;
   while(len--)
   {
    if(outbyte(s, s->out[s->outlen - dist])) return s->error;
   }
  }
 }
}

static int inflate_any(const uint8_t *in, uint32_t inlen, uint8_t **out, uint32_t *outlen, uint32_t *used, uint32_t limit)
{
 struct inflater s;
 struct huff lh, dh;
 int last;

 memset(&s, 0, sizeof(s));
 s.in = in;
 s.inlen = inlen;
 s.limit = limit;
 do
 {
  int type;
  last = (int)bits(&s, 1);
  type = (int)bits(&s, 2);
  if(s.error) break;
  if(type == 0)
  {
   uint32_t n, nn;
   s.bitbuf = 0;
   s.bitcount = 0;
   if(s.pos + 4 > inlen)
   {
    s.error = -1;
    break;
   }
   n = in[s.pos] | (in[s.pos + 1] << 8);
   nn = in[s.pos + 2] | (in[s.pos + 3] << 8);
   s.pos += 4;
   if(n != (~nn & 0xFFFF) || s.pos + n > inlen)
   {
    s.error = -1;
    break;
   }
   while(n--) if(outbyte(&s, in[s.pos++])) break;
  }
  else if(type == 1)
  {
   uint8_t len[288];
   int i;
   for(i = 0; i < 144; i++) len[i] = 8;
   for(; i < 256; i++) len[i] = 9;
   for(; i < 280; i++) len[i] = 7;
   for(; i < 288; i++) len[i] = 8;
   build(&lh, len, 288);
   for(i = 0; i < 30; i++) len[i] = 5;
   build(&dh, len, 30);
   s.error = codes(&s, &lh, &dh);
  }
  else if(type == 2)
  {
   uint8_t len[320], cl[19];
   struct huff ch;
   int nlit = (int)bits(&s, 5) + 257, ndist = (int)bits(&s, 5) + 1, ncl = (int)bits(&s, 4) + 4, i;
   if(s.error || nlit > 286 || ndist > 30)
   {
    s.error = -1;
    break;
   }
   memset(cl, 0, sizeof(cl));
   for(i = 0; i < ncl; i++) cl[ClOrder[i]] = (uint8_t)bits(&s, 3);
   if(s.error || build(&ch, cl, 19))
   {
    s.error = -1;
    break;
   }
   for(i = 0; i < nlit + ndist && !s.error;)
   {
    int sym = decode(&s, &ch), rep = 0;
    uint8_t val = 0;
    if(sym < 0) break;
    if(sym < 16)
    {
     len[i++] = (uint8_t)sym;
     continue;
    }
    if(sym == 16)
    {
     if(i == 0)
     {
      s.error = -1;
      break;
     }
     val = len[i - 1];
     rep = 3 + (int)bits(&s, 2);
    }
    else if(sym == 17) rep = 3 + (int)bits(&s, 3);
    else rep = 11 + (int)bits(&s, 7);
    if(i + rep > nlit + ndist)
    {
     s.error = -1;
     break;
    }
    while(rep--) len[i++] = val;
   }
   if(s.error || build(&lh, len, nlit) || build(&dh, len + nlit, ndist))
   {
    s.error = -1;
    break;
   }
   s.error = codes(&s, &lh, &dh);
  }
  else
  {
   s.error = -1;
  }
 }
 while(!last && !s.error);

 if(s.error == 1) s.error = 0;   /* stopped at the limit */
 if(s.error)
 {
  free(s.out);
  return s.error;
 }
 *out = s.out ? s.out : malloc(1);
 *outlen = s.outlen;
 if(used) *used = s.pos;
 return *out ? 0 : -2;
}

int inflate_raw(const uint8_t *in, uint32_t inlen, uint8_t **out, uint32_t *outlen, uint32_t *used)
{
 return inflate_any(in, inlen, out, outlen, used, 0);
}

int inflate_head(const uint8_t *in, uint32_t inlen, uint8_t **out, uint32_t *outlen, uint32_t limit)
{
 uint32_t used;
 return inflate_any(in, inlen, out, outlen, &used, limit);
}
