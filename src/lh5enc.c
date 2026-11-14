/*
** File:        lh5enc.c
** Description: -lh5- compressor (LZ77 with an 8 KB window + static Huffman
**              blocks), producing the bit stream that every LHA unpacker
**              reads. Written from the format the -lh5- decoders read
**              (the method of Haruhiko Okumura's public domain ar002).
**
**  A block is: number of codes (16 bits), the code lengths of the tree
**  used to send the literal/length code lengths ("pt", NT symbols), the
**  literal/length code lengths (NC symbols), the distance code lengths
**  (NP symbols), then the codes. Literal/length code c < 256 is a byte,
**  else a match of c - 253 bytes followed by its distance: the distance
**  code is the bit length of (distance - 1), then the bits below the top
**  one. All codes are canonical Huffman codes, sent most significant bit
**  first.
**
** Change history:
**  2026-11-14 orgin, created
*/

#include <stdlib.h>
#include <string.h>

#include "lha.h"

#define DICBIT    13
#define DICSIZ    (1 << DICBIT)     /* 8192 */
#define MAXMATCH  256
#define THRESHOLD 3
#define NC        (255 + MAXMATCH + 2 - THRESHOLD)   /* 510 */
#define CBIT      9
#define NP        (DICBIT + 1)      /* 14 */
#define PBIT      4
#define NT        19
#define TBIT      5
#define MAXLEN    16                /* longest Huffman code */

#define HASHBITS  15
#define HASHSIZE  (1 << HASHBITS)
#define MAXCHAIN  256
#define BLOCKMAX  16384             /* codes per block */

struct encoder
{
 /* output */
 uint8_t *out;
 uint32_t outpos, outmax;
 uint32_t bitbuf;
 int bitcount;
 int overflow;

 /* codes of the current block */
 uint16_t code[BLOCKMAX];           /* literal/length code */
 uint16_t dist[BLOCKMAX];           /* distance - 1 for matches */
 uint32_t ncodes;

 uint16_t c_freq[NC], p_freq[NP], t_freq[NT];
 uint8_t c_len[NC], p_len[NP], t_len[NT];
 uint16_t c_code[NC], p_code[NP], t_code[NT];

 /* match finder */
 uint32_t head[HASHSIZE];           /* position + 1 of the latest string with this hash, 0 = none */
 uint32_t prev[DICSIZ];             /* previous position + 1 with the same hash */
};

/*
** Send the n low bits of x, most significant first (n <= 16)
*/
static void putbits(struct encoder *e, int n, uint32_t x)
{
 if(n == 0) return;
 e->bitbuf = (e->bitbuf << n) | (x & ((1u << n) - 1));
 e->bitcount += n;
 while(e->bitcount >= 8)
 {
  e->bitcount -= 8;
  if(e->outpos >= e->outmax)
  {
   e->overflow = 1;
   return;
  }
  e->out[e->outpos++] = (uint8_t)(e->bitbuf >> e->bitcount);
 }
}

static void flushbits(struct encoder *e)
{
 if(e->bitcount > 0) putbits(e, 8 - e->bitcount, 0);
}

/*
** Huffman code lengths for freq[0..n-1] into len, no code longer than
** MAXLEN. Returns the number of symbols used; with one symbol used its
** length is 0 (the caller sends it as a single-symbol tree).
*/
static int make_lengths(const uint16_t *freq, int n, uint8_t *len, int *only)
{
 uint32_t f[2 * NC];
 int parent[2 * NC], used = 0, i, nodes, maxl;
 uint32_t scale = 0;

 for(i = 0; i < n; i++)
 {
  len[i] = 0;
  if(freq[i])
  {
   used++;
   *only = i;
  }
 }
 if(used < 2) return used;

 for(;;)
 {
  /* build the tree by repeatedly joining the two smallest nodes (n <= 510,
     so a simple search is fast enough) */
  int alive[2 * NC];
  nodes = n;
  for(i = 0; i < n; i++)
  {
   f[i] = freq[i] ? ((freq[i] >> scale) | 1) : 0;
   alive[i] = freq[i] != 0;
   parent[i] = -1;
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
   if(b < 0) break;   /* a is the root */
   f[nodes] = f[a] + f[b];
   alive[nodes] = 1;
   parent[nodes] = -1;
   alive[a] = alive[b] = 0;
   parent[a] = parent[b] = nodes;
   nodes++;
  }
  maxl = 0;
  for(i = 0; i < n; i++)
  {
   int l = 0, p;
   if(!freq[i]) continue;
   for(p = parent[i]; p >= 0; p = parent[p]) l++;
   len[i] = (uint8_t)l;
   if(l > maxl) maxl = l;
  }
  if(maxl <= MAXLEN) return used;
  scale++;   /* flatten the frequencies and try again */
 }
}

/*
** Canonical codes for the lengths: shorter codes first, equal lengths in
** symbol order (the order the decoders' table builder uses)
*/
static void make_codes(const uint8_t *len, int n, uint16_t *code)
{
 uint16_t count[MAXLEN + 2], next[MAXLEN + 2];
 int i;

 memset(count, 0, sizeof(count));
 for(i = 0; i < n; i++) count[len[i]]++;
 count[0] = 0;
 next[1] = 0;
 for(i = 1; i <= MAXLEN; i++) next[i + 1] = (uint16_t)((next[i] + count[i]) << 1);
 for(i = 0; i < n; i++)
 {
  if(len[i]) code[i] = next[len[i]]++;
 }
}

/*
** Frequencies of the "pt" symbols needed to send the c lengths:
** 0..2 code runs of zero lengths, k + 2 a length k
*/
static void count_t_freq(struct encoder *e, int n)
{
 int i = 0;
 memset(e->t_freq, 0, sizeof(e->t_freq));
 while(i < n)
 {
  int k = e->c_len[i++];
  if(k == 0)
  {
   int count = 1;
   while(i < n && e->c_len[i] == 0)
   {
    i++;
    count++;
   }
   if(count <= 2) e->t_freq[0] += (uint16_t)count;
   else if(count <= 18) e->t_freq[1]++;
   else if(count == 19)
   {
    e->t_freq[0]++;
    e->t_freq[1]++;
   }
   else e->t_freq[2]++;
  }
  else
  {
   e->t_freq[k + 2]++;
  }
 }
}

/*
** Send a pt length table: lengths up to 6 in 3 bits, longer ones as
** "111" followed by (length - 7) one bits and a zero. After the
** i_special'th length a 2-bit count of following zero lengths.
*/
static void write_pt_len(struct encoder *e, const uint8_t *len, int n, int nbit, int i_special)
{
 int i = 0;
 while(n > 0 && len[n - 1] == 0) n--;
 putbits(e, nbit, (uint32_t)n);
 while(i < n)
 {
  int k = len[i++];
  if(k <= 6) putbits(e, 3, (uint32_t)k);
  else putbits(e, k - 3, (1u << (k - 3)) - 2);
  if(i == i_special)
  {
   while(i < 6 && i < n && len[i] == 0) i++;
   putbits(e, 2, (uint32_t)(i - 3) & 3);
  }
 }
}

/*
** Send the c lengths using the pt codes
*/
static void write_c_len(struct encoder *e, int n)
{
 int i = 0;
 while(n > 0 && e->c_len[n - 1] == 0) n--;
 putbits(e, CBIT, (uint32_t)n);
 while(i < n)
 {
  int k = e->c_len[i++];
  if(k == 0)
  {
   int count = 1;
   while(i < n && e->c_len[i] == 0)
   {
    i++;
    count++;
   }
   if(count <= 2)
   {
    int j;
    for(j = 0; j < count; j++) putbits(e, e->t_len[0], e->t_code[0]);
   }
   else if(count <= 18)
   {
    putbits(e, e->t_len[1], e->t_code[1]);
    putbits(e, 4, (uint32_t)(count - 3));
   }
   else if(count == 19)
   {
    putbits(e, e->t_len[0], e->t_code[0]);
    putbits(e, e->t_len[1], e->t_code[1]);
    putbits(e, 4, 15);
   }
   else
   {
    putbits(e, e->t_len[2], e->t_code[2]);
    putbits(e, CBIT, (uint32_t)(count - 20));
   }
  }
  else
  {
   putbits(e, e->t_len[k + 2], e->t_code[k + 2]);
  }
 }
}

/*
** Number of bits of x (0 for 0)
*/
static int bitlen(uint32_t x)
{
 int n = 0;
 while(x)
 {
  n++;
  x >>= 1;
 }
 return n;
}

/*
** Send the codes collected so far as one block
*/
static void send_block(struct encoder *e)
{
 uint32_t i;
 int only = 0, used;

 if(e->ncodes == 0) return;
 putbits(e, 16, e->ncodes);

 /* literal/length tree, sent through the pt tree */
 used = make_lengths(e->c_freq, NC, e->c_len, &only);
 if(used >= 2)
 {
  int tonly = 0, tused;
  make_codes(e->c_len, NC, e->c_code);
  count_t_freq(e, NC);
  tused = make_lengths(e->t_freq, NT, e->t_len, &tonly);
  if(tused >= 2)
  {
   make_codes(e->t_len, NT, e->t_code);
   write_pt_len(e, e->t_len, NT, TBIT, 3);
  }
  else
  {
   memset(e->t_len, 0, sizeof(e->t_len));
   putbits(e, TBIT, 0);
   putbits(e, TBIT, (uint32_t)tonly);
  }
  write_c_len(e, NC);
 }
 else
 {
  /* a single literal/length code: sent with no bits */
  memset(e->c_len, 0, sizeof(e->c_len));
  putbits(e, TBIT, 0);
  putbits(e, TBIT, 0);
  putbits(e, CBIT, 0);
  putbits(e, CBIT, (uint32_t)only);
 }

 /* distance tree */
 only = 0;
 used = make_lengths(e->p_freq, NP, e->p_len, &only);
 if(used >= 2)
 {
  make_codes(e->p_len, NP, e->p_code);
  write_pt_len(e, e->p_len, NP, PBIT, -1);
 }
 else
 {
  memset(e->p_len, 0, sizeof(e->p_len));
  putbits(e, PBIT, 0);
  putbits(e, PBIT, (uint32_t)only);
 }

 for(i = 0; i < e->ncodes && !e->overflow; i++)
 {
  uint16_t c = e->code[i];
  putbits(e, e->c_len[c], e->c_code[c]);
  if(c >= 256)
  {
   uint32_t p = e->dist[i];
   int pb = bitlen(p);
   putbits(e, e->p_len[pb], e->p_code[pb]);
   if(pb > 1) putbits(e, pb - 1, p);
  }
 }

 e->ncodes = 0;
 memset(e->c_freq, 0, sizeof(e->c_freq));
 memset(e->p_freq, 0, sizeof(e->p_freq));
}

static void put_literal(struct encoder *e, uint8_t c)
{
 e->code[e->ncodes] = c;
 e->c_freq[c]++;
 if(++e->ncodes == BLOCKMAX) send_block(e);
}

static void put_match(struct encoder *e, uint32_t length, uint32_t distance)
{
 uint32_t p = distance - 1;
 uint16_t c = (uint16_t)(length + 256 - THRESHOLD);
 e->code[e->ncodes] = c;
 e->dist[e->ncodes] = (uint16_t)p;
 e->c_freq[c]++;
 e->p_freq[bitlen(p)]++;
 if(++e->ncodes == BLOCKMAX) send_block(e);
}

static uint32_t hash3(const uint8_t *p)
{
 return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) & (HASHSIZE - 1);
}

static void insert(struct encoder *e, const uint8_t *in, uint32_t pos, uint32_t len)
{
 if(pos + THRESHOLD <= len)
 {
  uint32_t h = hash3(in + pos);
  e->prev[pos & (DICSIZ - 1)] = e->head[h];
  e->head[h] = pos + 1;
 }
}

/*
** Longest match for in[pos..] in the window. Returns its length (0 when
** shorter than THRESHOLD), the distance in *distance.
*/
static uint32_t find_match(struct encoder *e, const uint8_t *in, uint32_t pos, uint32_t len, uint32_t *distance)
{
 uint32_t best = 0, cand, chain = MAXCHAIN;
 uint32_t maxlen = len - pos > MAXMATCH ? MAXMATCH : len - pos;

 if(maxlen < THRESHOLD) return 0;
 cand = e->head[hash3(in + pos)];
 while(cand && chain--)
 {
  uint32_t c = cand - 1, l = 0;
  if(c >= pos || pos - c > DICSIZ) break;
  if(in[c + best] == in[pos + best])
  {
   while(l < maxlen && in[c + l] == in[pos + l]) l++;
   if(l > best)
   {
    best = l;
    *distance = pos - c;
    if(l == maxlen) break;
   }
  }
  cand = e->prev[c & (DICSIZ - 1)];
 }
 return best >= THRESHOLD ? best : 0;
}

/*
** Compress len bytes of in with -lh5- into out (len bytes available).
** Returns the compressed size, or 0 when it isn't smaller than len.
*/
uint32_t lh5_compress(const uint8_t *in, uint32_t len, uint8_t *out)
{
 struct encoder *e = calloc(1, sizeof(*e));
 uint32_t pos = 0, result;

 if(e == NULL || len == 0)
 {
  free(e);
  return 0;
 }
 e->out = out;
 e->outmax = len;

 while(pos < len && !e->overflow)
 {
  uint32_t distance = 0, l = find_match(e, in, pos, len, &distance);
  if(l)
  {
   /* one step lazy: a longer match at the next byte wins */
   uint32_t d2 = 0, l2 = 0;
   if(pos + 1 < len)
   {
    insert(e, in, pos, len);
    l2 = find_match(e, in, pos + 1, len, &d2);
    if(l2 > l)
    {
     put_literal(e, in[pos]);
     pos++;
     l = l2;
     distance = d2;
    }
    else
    {
     /* pos is already inserted */
     put_match(e, l, distance);
     {
      uint32_t k;
      for(k = 1; k < l; k++) insert(e, in, pos + k, len);
     }
     pos += l;
     continue;
    }
   }
   put_match(e, l, distance);
   {
    uint32_t k;
    for(k = 0; k < l; k++) insert(e, in, pos + k, len);
   }
   pos += l;
  }
  else
  {
   insert(e, in, pos, len);
   put_literal(e, in[pos]);
   pos++;
  }
 }
 if(!e->overflow) send_block(e);
 if(!e->overflow) flushbits(e);
 result = (e->overflow || e->outpos >= len) ? 0 : e->outpos;
 free(e);
 return result;
}
