/* deflate_test MODE IN OUT: MODE c = compress IN to raw deflate OUT,
   d = inflate raw deflate IN to OUT. Used by tests/run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "../../src/deflate.h"
int main(int argc, char **argv)
{
 FILE *f; long n; uint8_t *in, *out; uint32_t ol, used;
 if(argc != 4) return 2;
 f = fopen(argv[2], "rb"); if(!f) return 2;
 fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
 in = malloc(n ? n : 1); if(fread(in, 1, n, f) != (size_t)n) return 2; fclose(f);
 if(argv[1][0] == 'c')
 {
  uint32_t max = deflate_bound((uint32_t)n);
  out = malloc(max);
  ol = deflate_compress(in, (uint32_t)n, out, max);
  if(ol == 0 && n) return 1;
 }
 else if(inflate_raw(in, (uint32_t)n, &out, &ol, &used) != 0) return 1;
 f = fopen(argv[3], "wb"); fwrite(out, 1, ol, f); fclose(f);
 return 0;
}
