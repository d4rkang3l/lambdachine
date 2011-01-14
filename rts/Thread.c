#include "Thread.h"
#include "Capability.h"
#include "MiscClosures.h"

#define MIN_STACK_WORDS 64

Thread *
createThread(Capability *cap, u4 size)
{
  Thread *T;
  u4 stack_size;

  if (size < MIN_STACK_WORDS + THREAD_STRUCT_SIZEW) {
    size = MIN_STACK_WORDS + THREAD_STRUCT_SIZEW;
  }

  // TODO: maybe round size to mem manager block size
  T = (Thread *)allocate(cap, size);
  stack_size = size - THREAD_STRUCT_SIZEW;

  T->header = 42;  // TODO
  T->stack_size = stack_size;
  T->stack[0] = (Word)&stg_STOP_closure;
  T->stack[1] = (Word)NULL;
  T->base = &T->stack[1];
  T->top = &T->stack[2];
  T->pc = getFInfo(&stg_STOP_closure)->code.code;

  return T;
}
