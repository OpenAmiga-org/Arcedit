/*
** holdlock FILE: keep an exclusive lock on FILE until Ctrl-C (a locked
** archive test for arcedit).
*/
#include <stdio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
 BPTR lock;
 if(argc < 2) return 20;
 lock = IDOS->Lock(argv[1], EXCLUSIVE_LOCK);
 if(!lock)
 {
  IDOS->PrintFault(IDOS->IoErr(), argv[1]);
  return 20;
 }
 printf("holdlock: %s locked\n", argv[1]);
 fflush(stdout);
 IExec->Wait(SIGBREAKF_CTRL_C);
 IDOS->UnLock(lock);
 return 0;
}
