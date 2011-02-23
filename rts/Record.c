#include "Common.h"
#include "IR.h"
#include "Jit.h"
#include "PrintIR.h"
#include "InfoTables.h"
#include "MiscClosures.h"
#include "Thread.h"
#include "Snapshot.h"
#include "HeapInfo.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* #define DBG_PR(fmt, ...)  fprintf(stderr, fmt, __VA_ARGS__) */


// -- Forward Declarations -------------------------------------------



// -- Convenience macros.  Undefined at end of file. -----------------

// Pointer to referenced IR.
#define IR(ref)     (&J->cur.ir[(ref)])
// The instruction currently being optimised
#define foldIns     (&J->fold.ins)

// -------------------------------------------------------------------

TRef
emitLoadSlot(JitState *J, i4 slot)
{
  TRef ref = emit_raw(J, IRT(IR_SLOAD, IRT_UNK), (i4)J->baseslot + slot, 0);
  J->base[slot] = ref;
  //  if (slot >= J->maxslot) J->maxslot = slot + 1;
  return ref;
}


// Return reference location of next instruction.
//
// Grows IR buffer if needed.
INLINE_HEADER IRRef
nextIns(JitState *J)
{
  IRRef ref = J->cur.nins;
  if (LC_UNLIKELY(ref >= J->irmax)) growIRBufferTop(J);
  J->cur.nins = ref + 1;
  return ref;
}

// Write instruction from Fold engine IR into IR buffer.
LC_FASTCALL TRef
emitIR(JitState *J)
{
  IROp op = foldIns->o;

  if (ir_mode[op] & IRM_G)
    addSnapshot(J);

  IRRef ref = nextIns(J);
  IRIns *ir = IR(ref);

  // Link into per-opcode chain.
  ir->prev = J->chain[op];
  J->chain[op] = (IRRef1)ref;
  ir->o = op;
  ir->t = foldIns->t;
  ir->op1 = foldIns->op1;
  ir->op2 = foldIns->op2;
  printf("emitted: %5d ", ref - REF_BIAS);
  printIR(J, *ir);

  return TREF(ref, ir->t);
}

// Common subexpression elimination.
//
// WARNING: CSE should not be done for side-effecting operations
// (including allocation).
LC_FASTCALL TRef
optCSE(JitState *J)
{
  IRRef2 op12 = (IRRef2)foldIns->op1 + ((IRRef2)foldIns->op2 << 16);
  IROp op = foldIns->o;
  if (1) {  // TODO: Test whether CSE is enabled
    IRRef ref = J->chain[op];  // first instruction with this opcode
    // There can be no duplicate beyond any argument.
    IRRef lim = foldIns->op2 > foldIns->op1 ? foldIns->op2 : foldIns->op1;

    while (ref > lim) {
      if (IR(ref)->op12 == op12) {
        // Common subexpression found
        DBG_PR("CSE: found duplicate of %d\n", (IRRef1)ref - REF_BIAS);
        return TREF(ref, IR(ref)->t);
      }
      ref = IR(ref)->prev;
    }
  }
  return emitIR(J);
}

#define IR_INITIAL_BUF_SIZE   256

// Grow IR buffer at the top.  Makes room for more instructions to be
// inserted.
void
growIRBufferTop(JitState *J)
{
  IRIns *baseir = J->irbuf + J->irmin;
  Word szins = J->irmax - J->irmin;

  if (szins != 0) {
    baseir = realloc(baseir, 2 * szins * sizeof(IRIns));
    J->irmax = J->irmin + 2 * szins;
    DBG_PR(COLOURED(COL_BLUE, "Resizing IR buffer to: %d..%d\n"),
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  } else {
    baseir = xmalloc(IR_INITIAL_BUF_SIZE * sizeof(IRIns));
    J->irmin = REF_BASE - IR_INITIAL_BUF_SIZE/4;
    J->irmax = J->irmin + IR_INITIAL_BUF_SIZE;
    DBG_PR(COLOURED(COL_BLUE, "Alloc new IR buf: %d..%d\n"),
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  }
  J->cur.ir = J->irbuf = baseir - J->irmin;
}

// Grow IR buffer at the bottom.  Makes room for more literals.
// May choose to move existing content instead of resizing buffer.
static void
growIRBufferBottom(JitState *J)
{
  IRIns *baseir = J->irbuf + J->irmin;
  Word szins = J->irmax - J->irmin;
  LC_ASSERT(szins != 0);
  LC_ASSERT(J->cur.nk == J->irmin);

  if (J->cur.nins + (szins >> 1) < J->irmax) {
    // More than half of the buffer is free on top end.  Shift up by a
    // quarter.
    Word ofs = szins >> 2;
    DBG_PR(COLOURED(COL_BLUE, "Shifting IRs up by %" FMT_Word "\n"),
           ofs);
    memmove(baseir + ofs, baseir, (J->cur.nins - J->irmin) * sizeof(IRIns));
    J->irmin -= ofs;
    J->irmax -= ofs;
    J->cur.ir = J->irbuf = baseir - J->irmin;
  } else {
    // Double buffer size.
    IRIns *newbaseir = xmalloc(2 * szins * sizeof(IRIns));
    // Limit bottom growth.  Thus makes room for at most 128 new literals.
    Word ofs = szins >= 256 ? 128 : (szins >> 1);
    memcpy(newbaseir + ofs, baseir,
           (J->cur.nins - J->irmin) * sizeof(IRIns));
    free(baseir);
    J->irmin -= ofs;
    J->irmax = J->irmin + 2 * szins;
    J->cur.ir = J->irbuf = newbaseir - J->irmin;
    DBG_PR(COLOURED(COL_BLUE, "Growing IR buffer to: %d..%d\n"),
           J->irmin - REF_BIAS, J->irmax - REF_BIAS);
  }
}

void
recordSetup(JitState *J, Thread *T)
{
  memset(J->slot, 0, sizeof(J->slot));
  memset(J->chain, 0, sizeof(J->chain));

  J->baseslot = 1;  // baseslot[0] == base[-1] == Node
  J->base = J->slot + J->baseslot;
  J->maxslot = T->top - T->base;

  J->T = T;

  J->flags = 0; // TODO: Default flags

  J->irmin = 0;
  J->irmax = 0;
  J->cur.ir = J->irbuf = NULL;
  J->cur.nins = REF_BASE;
  J->cur.nk = REF_BASE;
  J->cur.nloop = 0;
  // Emit BASE.  Causes buffer allocation.
  emit_raw(J, IRT(IR_BASE, IRT_PTR), 0, 0);
  J->last_result = 0;
  J->framedepth = 0;

  J->maxkwords = 16;
  J->kwords = xmalloc(J->maxkwords * sizeof(Word));
  J->nextkword = 0;

  J->needsnap = 0;
  J->mergesnap = 1;
  J->cur.nsnap = J->sizesnap = 0;
  J->cur.nsnapmap = J->sizesnapmap = 0;
  J->cur.snap = J->snapbuf = NULL;
  J->cur.snapmap = J->snapmapbuf = NULL;

  J->cur.nheap = J->sizeheap = 0;
  J->cur.nheapmap = J->sizeheapmap = 0;
  J->cur.heap = J->heapbuf = NULL;
  J->cur.heapmap = J->heapmapbuf = NULL;

  J->needsnap = 0;
}

INLINE_HEADER IRRef
nextLit(JitState *J)
{
  IRRef ref = J->cur.nk;
  if (LC_UNLIKELY(ref <= J->irmin)) growIRBufferBottom(J);
  J->cur.nk = --ref;
  return ref;
}

TRef
emitKInt(JitState *J, i4 k)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;
  // Try to find in existing constants
  for (ref = J->chain[IR_KINT]; ref; ref = cir[ref].prev)
    if (cir[ref].i == k)
      goto found;
  ref = nextLit(J);
  ir = IR(ref);
  ir->i = k;
  ir->t = IRT_I32;
  ir->o = IR_KINT;
  ir->prev = J->chain[IR_KINT];
  J->chain[IR_KINT] = (IRRef1)ref;
  printf("emitted: %5d ", (IRRef1)ref - REF_BIAS);
  printIR(J, *ir);
 found:
  return TREF(ref, IRT_I32);
}

INLINE_HEADER IRType
litTypeToIRType(LitType lt)
{
  switch (lt) {
  case LIT_INT:     return IRT_I32;
  case LIT_STRING:  return IRT_PTR;
  case LIT_CHAR:    return IRT_I32;
  case LIT_WORD:    return IRT_U32;
  case LIT_FLOAT:   return IRT_F32;
  case LIT_INFO:    return IRT_INFO;
  case LIT_CLOSURE: return IRT_CLOS;
  case LIT_PC:      return IRT_PC;
  default: LC_ASSERT(0); return 0;
  }
}

LC_FASTCALL
TRef
emitKWord(JitState *J, Word w, LitType lt)
{
  IRIns *ir, *cir = J->cur.ir;
  Word *kword = J->kwords;
  IRRef ref;
  IRType t = litTypeToIRType(lt);

  for (ref = J->chain[IR_KWORD]; ref; ref = cir[ref].prev)
    if (cir[ref].t == t && kword[cir[ref].u] == w)
      goto found;

  if (LC_UNLIKELY(J->nextkword >= J->maxkwords)) {
    J->maxkwords *= 2;
    J->kwords = realloc(J->kwords, J->maxkwords);
  }

  ref = nextLit(J);
  ir = IR(ref);
  ir->u = J->nextkword;
  J->kwords[J->nextkword] = w;
  J->nextkword++;
  ir->t = t;
  ir->o = IR_KWORD;
  ir->prev = J->chain[IR_KWORD];
  J->chain[IR_KWORD] = (IRRef1)ref;
  printf("emitted: %5d ", (IRRef1)ref - REF_BIAS);
  printIR(J, *ir);
 found:
  return TREF(ref, t);
}

// Emit a constant representing a pointer relative to the trace base.
//
// This is used to build stack frames in the trace recorder.  Stack frames
// currently store:  the return address and a pointer to the previous base.
//
// For the return address we can use Word-sized constants since we're
// expecting to return to the same bytecode instruction.  The pointer
// to the previous base, however, depends on the current stack depth
// (and memory location of the stack).  We don't want to specialise on
// that.  Instead we want to store pointers relative to the value of
// `base` when entering the trace.
//
// Of course, any of these values will only end up being written to memory
// when exiting a trace.
//
LC_FASTCALL TRef
emitKBaseOffset(JitState *J, i4 offs)
{
  IRIns *ir, *cir = J->cur.ir;
  IRRef ref;

  for (ref = J->chain[IR_KBASEO]; ref; ref = cir[ref].prev)
    if (cir[ref].i == offs)
      goto found;

  ref = nextLit(J);
  ir = IR(ref);
  ir->i = offs;
  ir->o = IR_KBASEO;
  ir->t = IRT_PTR;
  ir->prev = J->chain[IR_KBASEO];
  J->chain[IR_KBASEO] = ir->prev;
  printf("emitted: %5d ", (IRRef1)ref - REF_BIAS);
  printIR(J, *ir);
 found:
  return TREF(ref, ir->t);
}

// Emit load of a field.
INLINE_HEADER TRef
emitFLoad(JitState *J, TRef ptr, u2 offset)
{
  TRef ref = emit(J, IRT(IR_FREF, IRT_PTR), ptr, offset);
  return emit(J, IRT(IR_FLOAD, IRT_UNK), ref, 0);
}

INLINE_HEADER TRef
emitInfoTableGuard(JitState *J, TRef clos, InfoTable *info)
{
  return 0;
}

INLINE_HEADER void
guardEqualKWord(JitState *J, TRef ref, Word k, LitType lt)
{
  TRef kref = emitKWord(J, k, lt);
  emit(J, IRT(IR_EQ, IRT_VOID), ref, kref);
}

void
printSlots(JitState *J)
{
  //printf("slots: %d, %d\n", J->baseslot, J->maxslot);
  u4 i, j;
  for (i = 0; i < J->baseslot + J->maxslot; i++) {
    IRRef1 ref = J->slot[i];

    j = i - J->baseslot;
    if ((j & 0x03) == 0)
      printf("[%d]:", j);

    if (ref == 0)
      printf("---- ");
    else if (ref >= REF_BIAS)
      printf("%04d ", ref - REF_BIAS);
    else
      printf("K%03d ", REF_BIAS - ref);
  }
  printf("\n");
}

void
printIRBuffer(JitState *J)
{
  IRRef ref;
  SnapShot *snap = J->cur.nsnap > 0 ? J->cur.snap : NULL;
  IRRef nextsnap = snap ? snap->ref : 0;

  printf("IRs (%d..%d):\n",
         J->cur.nk - REF_BIAS,
         J->cur.nins - REF_BIAS);

  for (ref = J->cur.nk; ref < J->cur.nins; ref++) {
    if (ref == nextsnap) {
      printf("          ");
      printSnapshot(J, snap, J->cur.snapmap);
      ++snap;
      if (snap >= J->cur.snap + J->cur.nsnap) {
        snap = NULL; nextsnap = 0;
      } else 
        nextsnap = snap->ref;
    }
    printIRRef(J, ref);
    printIR(J, *IR(ref));
  }
}

int
evalNumComp(Word x, Word y, IROp op)
{
  switch (op) {
  case IR_EQ: return (x == y);
  case IR_NE: return (x != y);
  case IR_LT: return (x <  y);
  case IR_GE: return (x >= y);
  case IR_LE: return (x <= y);
  case IR_GT: return (x >  y);
  default: LC_ASSERT(0); return 0;
  }
}

void
abortRecording(JitState *J)
{
  LC_ASSERT(0); // TODO: Implement
}

int
recordIns(JitState *J)
{
  const BCIns *pc;
  Word *tbase;
  BCIns ins;
  BCOp op;
  TRef ra, rb, rc;

  if (LC_UNLIKELY(J->pc == J->startpc)) {
    if (J->mode == 1) J->mode = 2;
    else {
      finishRecording(J);
      return 0;
    }
  }


  if (J->needsnap) {
    J->needsnap = 0;
    printf(COL_GREEN);
    printSlots(J);
    addSnapshot(J);
    printf(COL_RESET);
    J->mergesnap = 1;
  }

  pc = J->pc;
  tbase = J->T->base;
  ins = *pc;
  op = bc_op(ins);

  switch (op) {

    // -- Comparisons ----------------------------------------------------

  case BC_ISLT: case BC_ISGE: case BC_ISLE: case BC_ISGT:
  case BC_ISEQ: case BC_ISNE:
    {
      DBG_PR("Emitting comparison: %s\n", ins_name[op]);
      // All treated as integer comparisions
      ra = getSlot(J, bc_a(ins));
      rc = getSlot(J, bc_d(ins));
      WordInt rav = tbase[bc_a(ins)];
      WordInt rcv = tbase[bc_d(ins)];
      int irop = (int)IR_LT + ((int)op - (int)BC_ISLT);
      // Invert condition if negative outcome
      if (!evalNumComp(rav, rcv, irop)) irop ^= 1;
      emit(J, IRT(irop, IRT_VOID), ra, rc);
    }
    break;

  case BC_MOV:
    {
      rc = getSlot(J, bc_d(ins));
      setSlot(J, bc_a(ins), rc);
    }
    break;

  case BC_MOV_RES:
    {
      if (!J->last_result)
        J->last_result = emit_raw(J, IRT(IR_RLOAD, IRT_UNK), 0, 0);
      setSlot(J, bc_a(ins), J->last_result);
      //printSlots(J);
    }
    break;

  case BC_ADDRR: case BC_SUBRR: case BC_MULRR:
    {
      rb = getSlot(J, bc_b(ins));
      rc = getSlot(J, bc_c(ins));
      int irop = (int)IR_ADD + ((int)op - (int)BC_ADDRR);
      setSlot(J, bc_a(ins), emit(J, IRT(irop, IRT_I32), rb, rc));
    }
    break;

  case BC_JMP:
    break;

  case BC_LOADK:
    {
      u2 l = bc_d(ins);
      rb = emitKWord(J, J->func->code.lits[l], J->func->code.littypes[l]);
      setSlot(J, bc_a(ins), rb);
      //printSlots(J);
    }
    break;

  case BC_LOADFV:
    {
      rb = getSlot(J, -1);
      ra = emitFLoad(J, rb, bc_d(ins));
      setSlot(J, bc_a(ins), ra);
    }
    break;

  case BC_LOADF:
    {
      rb = getSlot(J, bc_b(ins));
      ra = emitFLoad(J, rb, bc_c(ins));
      setSlot(J, bc_a(ins), ra);
    }
    break;

  case BC_EVAL:
    {
      //DBG_PR("Recording EVAL r%d\n", bc_a(ins));
      ra = getSlot(J, bc_a(ins));
      Closure* node = (Closure*)tbase[bc_a(ins)];
      const InfoTable *ninfo = getInfo(node);

      if (closure_IND(node)) {
        abortRecording(J); // TODO: Handle indirection following.
      }

      // Specialise on info table:  Emit guard to check for same info
      // table as the one we encountered at recording time.
      rc = emitKWord(J, (Word)ninfo, LIT_INFO);
      rb = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
      emit(J, IRT(IR_EQ, IRT_VOID), rb, rc);

      if (closure_HNF(node)) {
        // ra is in normal form.  Guard makes sure of that, so we now just
        // return.
        //
        // TODO: Actually, we may specialise a bit too much here.  It
        // depends on whether we do a case dispatch later on.
        J->last_result = ra;

      } else {
        //printSlots(J);
        // Setup stack frame for evaluation.
        Word *top = J->T->top;
        ThunkInfoTable *info = (ThunkInfoTable*)ninfo;
        u4 framesize = info->code.framesize;
        if (LC_UNLIKELY(stackOverflow(J->T, top, 8 + framesize)))
          abortRecording(J);
        u4 t = top - J->T->base; // Slot name of *top
        u4 i;
        const BCIns *return_pc = J->pc + 2;
        // TODO: undefine local slots that are not live-out
        setSlot(J, t + 0, emitKBaseOffset(J, 0));
        setSlot(J, t + 1, emitKWord(J, (Word)return_pc, LIT_PC));
        setSlot(J, t + 2, emitKWord(J, (Word)&stg_UPD_closure, LIT_CLOSURE));
        setSlot(J, t + 3, ra); // the thing to update
        setSlot(J, t + 4, 0); // undefined
        setSlot(J, t + 5, emitKBaseOffset(J, t + 3));
        setSlot(J, t + 6, emitKWord(J, (Word)stg_UPD_return_pc, LIT_PC));
        setSlot(J, t + 7, ra);
        printf("baseslot %d => %d (top = %d, frame = %d)\n",
               J->baseslot, J->baseslot + t + 8, t, framesize);
        J->baseslot += t + 8;
        J->base = J->slot + J->baseslot;
        J->maxslot = framesize;
        emit_raw(J, IRT(IR_FRAME, IRT_VOID), t + 8, framesize);
        for (i = 0; i < J->maxslot; i++) setSlot(J, i, 0); // clear slots
        J->framedepth += 2;
        printSlots(J);
      }
    }
    break;

  case BC_CALLT:
    {
      // TODO: For now only supports exact calls.
      u4 nargs = bc_d(ins);
      Closure *fnode = (Closure*)tbase[bc_a(ins)];
      FuncInfoTable *info;

      if (getInfo(fnode)->type != FUN ||
          getFInfo(fnode)->code.arity != nargs)
        abortRecording(J);

      info = getFInfo(fnode);
      // Guard for info table, as usual
      TRef rinfo = emitKWord(J, (Word)info, LIT_INFO);
      ra = getSlot(J, bc_a(ins));
      rb = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
      emit(J, IRT(IR_EQ, IRT_VOID), rb, rinfo);
      setSlot(J, -1, ra);

      J->maxslot = info->code.framesize;
      // Invalidate non-argument slots:
      u4 i;
      for (i = nargs; i < J->maxslot; i++) setSlot(J, i, 0);
      emit_raw(J, IRT(IR_FRAME, IRT_VOID), 0, J->maxslot);

      printSlots(J);
    }
    break;

  case BC_UPDATE:
    {
      ra = getSlot(J, bc_a(ins));
      rb = getSlot(J, bc_d(ins));
      emit(J, IRT(IR_UPDATE, IRT_VOID), ra, rb);
      J->last_result = rb;
      J->needsnap = 1;
      goto do_return;
    }

  case BC_RET1:
    {
      J->last_result = getSlot(J, bc_a(ins));


    do_return:
      if (J->framedepth <= 0)
        abortRecording(J); // for now

      Word return_pc = tbase[-2];
      Word *return_base = (Word*)tbase[-3];
      i4 basediff = tbase - return_base;
      int i;

      J->framedepth--;

      guardEqualKWord(J, getSlot(J, -2), return_pc, LIT_PC);

      for (i = -3; i < (int)J->maxslot; i++)
        J->base[i] = 0;

      // TODO: Do something with slot(-3)?
      J->baseslot -= basediff;
      J->base = J->slot + J->baseslot;
      J->maxslot = basediff - 3;
      emit_raw(J, IRT(IR_RET, IRT_VOID), basediff, 0);

      printSlots(J);
    }
    break;

  case BC_ALLOC1:
    {
      InfoTable *info = (InfoTable*)tbase[bc_b(ins)];
      TRef rinfo, rnew, rfield;
      rinfo = emitKWord(J, (Word)info, LIT_INFO);
      rb = getSlot(J, bc_b(ins));
      // Ensure that r(B) actually contains the the info table we're
      // expecting.  Usually, this will be optimised away.
      emit(J, IRT(IR_EQ, IRT_VOID), rb, rinfo);

      rc = getSlot(J, bc_c(ins));
      rnew = emit(J, IRT(IR_NEW, IRT_CLOS), rinfo, 2);
      rfield = emit(J, IRT(IR_FREF, IRT_PTR), rnew, 1);
      emit(J, IRT(IR_FSTORE, IRT_VOID), rfield, rc);
      setSlot(J, bc_a(ins), rnew);
      u4 h = newHeapInfo(J, rnew, info);
      IR(tref_ref(rnew))->op2 = h;
      setHeapInfoField(J, &J->cur.heap[h], 0, rc);
      //printSlots(J);
    }
    break;

  case BC_ALLOC:
    {
      InfoTable *info = (InfoTable*)tbase[bc_b(ins)];
      TRef rinfo, rnew, rfield;
      u4 size = bc_c(ins);
      ConInfoTable *cinfo;
      u1 *arg = (u1*)(pc + 1);
      u4 i;
      rinfo = emitKWord(J, (Word)info, LIT_INFO);
      rb = getSlot(J, bc_b(ins));
      emit(J, IRT(IR_EQ, IRT_VOID), rb, rinfo);
      cinfo = (ConInfoTable*)info;
      rnew = emit(J, IRT(IR_NEW, IRT_CLOS), rinfo,
                  cinfo->i.layout.payload.ptrs +
                  cinfo->i.layout.payload.nptrs + 1);
      u4 h = newHeapInfo(J, rnew, info);
      IR(tref_ref(rnew))->op2 = h;
      HeapInfo *hp = &J->cur.heap[h];
      for (i = 1; i <= size; i++, arg++) {
        rc = getSlot(J, *arg);
        rfield = emit(J, IRT(IR_FREF, IRT_PTR), rnew, i);
        emit(J, IRT(IR_FSTORE, IRT_VOID), rfield, rc);
        setHeapInfoField(J, hp, i - 1, rc);
      }
      setSlot(J, bc_a(ins), rnew);
      //printSlots(J);
    }
    break;

  case BC_CASE:
    {
      // The nice thing is, we don't care which case is selected, just
      // that the info table remains the same.
      ra = getSlot(J, bc_a(ins));
      Closure *cl = (Closure*)tbase[bc_a(ins)];
      ra = emit(J, IRT(IR_ILOAD, IRT_INFO), ra, 0);
      guardEqualKWord(J, ra, (Word)getInfo(cl), LIT_INFO);
    }
    break;

  default:
    DBG_PR("record: Ignoring %s\n", ins_name[op]);
    LC_ASSERT(0);
    break;
  }

  return 1;
}

void
initJitState(JitState *J)
{
  J->mode = 0;
  J->startpc = 0;
}

LC_FASTCALL void
startRecording(JitState *J, const BCIns *startpc, Thread *T, Word *base)
{
  printf("start recording: %p\n", T);
  T->base = base;
  J->startpc = J->cur.startpc = startpc;
  J->mode = 1;
  recordSetup(J, T);
  printf("*** Starting to record at: %p\n", startpc);
}

void
finishRecording(JitState *J)
{
  // int i;
  addSnapshot(J);
  J->cur.nloop = tref_ref(emit_raw(J, IRT(IR_LOOP, IRT_VOID), 0, 0));
  optUnrollLoop(J);
  optDeadCodeElim(J);
  heapSCCs(J);

  //  for (i = J->cur.nheap - 1; i >= 0; i--)
  //    heapSCCs(J, i);

  printf("*** Stopping to record.\n");
  printIRBuffer(J);
  printHeapInfo(J);
}

// Perform store->load forwarding on the current foldIns.
TRef
optForward(JitState *J)
{
  IROp op = foldIns->o;
  // There cannot be a store before the reference to the location is created.
  // (This relies on CSE to find duplicate references).
  IRRef lim = foldIns->op1;
  IRRef1 fref = lim;

  switch (op) {
  case IR_FLOAD:
    {
      //printf("FWD: looking for store to %d\n",  fref - REF_BIAS);
      IRRef ref = J->chain[IR_FSTORE];

      // Find a store to same address.
      while (ref > lim) {
        if (IR(ref)->op1 == fref) {
          IRRef val = IR(ref)->op2;
          printf("FWD: Forwarding load: %d (%d, %d)\n",
                 val - REF_BIAS, ref - REF_BIAS, fref - REF_BIAS);
          return TREF(val, IR(val)->t);
        }
        ref = IR(ref)->prev;
      }
      // TODO: Check for aliases?
      return optCSE(J);
    }
    break;
  default:
    break;
  }
  return emitIR(J);
}



TRef
foldIR(JitState *J)
{
  IROp op = foldIns->o;

 again:
  switch (op) {
  case IR_EQ:
    if (foldIns->op1 == foldIns->op2) {
      printf("FOLD: trivial guard.\n");
      return 0;
    }
    break;
  case IR_ILOAD:
  iload_again:
    if (irref_islit(foldIns->op1)) {
      IRIns ir = J->cur.ir[foldIns->op1];
      LC_ASSERT(ir.o == IR_KWORD);
      Closure *c = (Closure*)J->kwords[ir.u];
      printf("FOLD: ILOAD for static closure\n");
      return emitKWord(J, (Word)getFInfo(c), LIT_INFO);
    } else {
      IRIns *left = IR(foldIns->op1);
      if (left->o == IR_NEW) {
        printf("FOLD: ILOAD for NEW closure\n");
        return left->op1;
      }
    }
    break;
  case IR_SLOAD:
    if (J->slot[foldIns->op1])
      return J->slot[foldIns->op1];
    break;
  case IR_NEW:
    // TODO: Allocating a loop-invariant value only needs to be done
    // once.
    break;
  default:
    ;
  }

  if (ir_mode[op] & IRM_A) {
    // Don't optimise allocation, yet.
    return emitIR(J);
  } else if (ir_mode[op] & IRM_L && op != IR_ILOAD) {
    return optForward(J);
  } else {
    //printf("CSE on %s\n", ir_name[op]);
    return optCSE(J);
  }
}

void
optUnrollLoop(JitState *J)
{
  TRef *renaming;
  u4 max_renamings = J->cur.nloop - REF_BIAS;
  IRRef ref;
  u4 nextsnap = 0;

  // TODO: Keep track of PHI nodes

  DBG_LVL(2,"max_renamings = %u\n", max_renamings);
  renaming = xmalloc(max_renamings * sizeof(*renaming));
  renaming -= REF_BIAS;

  for (ref = REF_FIRST; ref < J->cur.nloop; ref++) {
    IRIns *ir = IR(ref);
    IRRef1 op1, op2;

# define RENAME(r) \
    (((r) < ref && (r) > REF_BIAS) ? \
      tref_ref(renaming[(r)]) : (r))

    DBG_LVL(2, "UNROLL: %s", "");
    IF_DBG_LVL(2, printIR(J, *ir));

    // If there is a snapshot at the current instruction we need to
    // "replay" it first.  Simply loop over the elements in the
    // snapshot and apply the substitution and modify the slot table.
    if (nextsnap < J->cur.nsnap && J->cur.snap[nextsnap].ref == ref) {
      SnapShot *snap = &J->cur.snap[nextsnap];
      SnapEntry *p = J->cur.snapmap + snap->mapofs;
      int i;
      for (i = 0; i < snap->nent; i++, p++) {
        IRRef1 r = RENAME(snap_ref(*p));
        IRIns *ir = IR(r);
        int slot = snap_slot(*p);
        //printf("setting slot %d, to %d, %d\n", slot, r - REF_BIAS, ir->t);
        J->slot[slot] = TREF(r, ir->t);
      }
      J->pc = J->cur.startpc + (ptrdiff_t)(i4)(*p);
      // base slot is updated separately via ENTER and RET instructions.
      nextsnap++;
    }

    // Apply renaming to operands if needed.
    //
    // If the operand is a reference and we already have a valid
    // renaming, then use that.  Otherwise, it remains unchanged.
    op1 = (irm_op1(ir_mode[ir->o]) == IRMref) ? RENAME(ir->op1) : ir->op1;
    op2 = (irm_op2(ir_mode[ir->o]) == IRMref) ? RENAME(ir->op2) : ir->op2;

    //printf("op1 = %d, op2 = %d\n", op1 - REF_BIAS, op2 - REF_BIAS);
    renaming[ref] = emit(J, IRT(ir->o, ir->t), op1, op2);

    // FRAME and RET instructions keep track of the
    switch (ir->o) {
    case IR_FRAME:
      J->baseslot += ir->op1;
      J->base = J->slot + J->baseslot;
      J->maxslot = ir->op2;
      break;
    case IR_RET:
      J->baseslot -= ir->op1;
      J->base = J->slot + J->baseslot;
      J->maxslot = ir->op1 - 3;
      break;
    case IR_NEW:
      {
        HeapInfo *hpold = &J->cur.heap[ir->op2];
        u2 entry = cloneHeapInfo(J, tref_ref(renaming[ref]), ir->op2);
        HeapInfo *hpnew = &J->cur.heap[entry];
        IRIns *ir2 = IR(tref_ref(renaming[ref]));
        u2 i;
        for (i = 0; i < hpnew->nfields; i++) {
          IRRef r = getHeapInfoField(J, hpold, i);
          setHeapInfoField(J, hpnew, i, RENAME(r));
        }
        ir2->op2 = entry;
      }
      break;
    default: break;
    }

    DBG_LVL(2, "   %d => ", ref - REF_BIAS);
    IF_DBG_LVL(2, printIRRef(J, tref_ref(renaming[ref])));
    DBG_LVL(2, "\n%s", "");
# undef RENAME
  }

  // Emit PHI instructions
  for (ref = REF_FIRST; ref < J->cur.nloop; ref++) {
    TRef tr = renaming[ref];
    IRIns *ir = IR(tref_ref(tr));
    if (tref_t(tr) != IRT_VOID && tref_ref(tr) > J->cur.nloop &&
        ir->o != IR_FREF) {
      irt_setphi(IR(ref)->t);
      emit(J, IRT(IR_PHI, ir->t), ref, tref_ref(tr));
      irt_setphi(ir->t);
    }
  }

  xfree(renaming + REF_BIAS);
}

// Find the corresponding twin of a referenced involved in a PHI node.
//
// For example:
//
//     t1  ADD a b
//     --- LOOP ---
//     x1  SUB t1 c  ; reference to t1 or t2
//     t2  ADD t1 b
//     x2  SUB t2 d  ; reference to just t2
//     -   PHI t1 t2
//
// The basic principle is that all PHI nodes semantically occur right
// after the LOOP marker.  The reference to `t1` in `x1` therefore refers
// to `t1` in the first iteration and thereafter to `t2` from the
// previous iteration.
//
LC_FASTCALL IRRef
findPhiTwin(JitState *J, IRRef ref)
{
  if (ref < J->cur.nloop && irt_getphi(IR(ref)->t)) {
    // We have a reference to a loop variant variable
    IRRef1 r = J->chain[IR_PHI];
    while (r) {
      IRIns *ir = IR(r);
      if (ir->op1 == ref)
	return ir->op2;
      r = ir->prev;
    }
    // We must have a matching PHI node if the IRT_PHI flag is set.
    // So we should never reach this point.
    LC_ASSERT(0);
  } else
    return 0;
}

INLINE_HEADER
void
markIRRef(JitState *J, IRRef ref, IRRef site)
{
  if (irref_islit(ref))
    return;

  IRIns *ir = IR(ref);

  // If we are marking a node involved in a PHI node, mark the
  // PHI node as well.  However, we only do this the first time
  // around, because marking a PHI node uses linear search.
  if (site > J->cur.nloop && irt_getphi(ir->t) && !irt_getmark(ir->t)) {
    IRRef phiref = tref_ref(J->chain[IR_PHI]);
    while (phiref) {
      if (IR(phiref)->op1 == ref) {
        irt_setmark(IR(phiref)->t);
        break;
      }
      phiref = IR(phiref)->prev;
    }
  }
  irt_setmark(ir->t);
}

// Dead code elimination.
//
// Marks all live-out references and their dependencies.  Anything not
// marked at the end is dead code and can be deleted (replaced by NOPs).
//
// Allocation sinking is done in a separate pass.
//
LC_FASTCALL void
optDeadCodeElim(JitState *J)
{
  SnapShot *snap;
  i4 i, j;
  // 1. Mark all variables mentioned in snapshots.
  //
  // We start from the end so that if a variable `x` is marked and `x`
  // is involved in a PHI node, then marked `IR(x)` implies marked
  // `PHI x`.
  for (i = J->cur.nsnap - 1; i >= 0; i--) {
    snap = &J->cur.snap[i];
    SnapEntry *se = &J->cur.snapmap[snap->mapofs];
    for (j = 0; j < snap->nent; j++, se++) {
      markIRRef(J, snap_ref(*se), snap->ref);
    }
  }

  // 2. Mark all instructions reachable from those marked above.
  //
  // Again we start from the back, because data dependencies point
  // backwards.
  IRRef ref;
  for (ref = J->cur.nins - 1; ref >= REF_FIRST; ref--) {
    IRIns *ir = IR(ref);
    if (ir_mode[ir->o] & (IRM_S|IRM_G))
      irt_setmark(ir->t);
    if (irt_getmark(ir->t)) {
      if (irm_op1(ir_mode[ir->o]) == IRMref)
        markIRRef(J, ir->op1, ref);
      if (irm_op2(ir_mode[ir->o]) == IRMref)
        markIRRef(J, ir->op2, ref);
    }
  }

  // 3. Replace all unmarked instructions by NOPs

  // TODO
}

#undef IR
#undef foldIns