/*
** memeater KB/N: allocate memory until only KB kilobytes are left free,
** hold it until Ctrl-C, then give it all back (a low memory test for
** arcedit). AmigaOS doesn't free AllocVec memory at exit, so the blocks
** are kept in a list (the first bytes of each block point to the next).
*/
#include <stdio.h>
#include <stdlib.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
 long keep = argc > 1 ? atol(argv[1]) * 1024 : 1024 * 1024;
 long total = 0;
 void *list = NULL;

 for(;;)
 {
  uint32 free = IExec->AvailMem(MEMF_ANY);
  uint32 chunk;
  void **p;
  if(free <= (uint32)keep) break;
  chunk = free - keep > 16 * 1024 * 1024 ? 16 * 1024 * 1024 : free - keep;
  if(chunk < 4096) break;
  p = IExec->AllocVecTags(chunk, AVT_Type, MEMF_ANY, TAG_DONE);
  if(p == NULL) break;
  *p = list;
  list = p;
  total += chunk;
 }
 printf("memeater: holding %ld KB, %lu KB free\n", total / 1024, (unsigned long)(IExec->AvailMem(MEMF_ANY) / 1024));
 fflush(stdout);
 IExec->Wait(SIGBREAKF_CTRL_C);
 while(list)
 {
  void *next = *(void **)list;
  IExec->FreeVec(list);
  list = next;
 }
 printf("memeater: memory given back\n");
 return 0;
}
