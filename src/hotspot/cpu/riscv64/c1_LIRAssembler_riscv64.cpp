/*
 * Copyright (c) 2000, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * Copyright (c) 2020, Huawei Technologies Co., Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/assembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "c1/c1_CodeStubs.hpp"
#include "c1/c1_Compilation.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_Runtime1.hpp"
#include "c1/c1_ValueStack.hpp"
#include "ci/ciArrayKlass.hpp"
#include "ci/ciInstance.hpp"
#include "code/compiledIC.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "nativeInst_riscv64.hpp"
#include "oops/objArrayKlass.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/macros.hpp"
#include "vmreg_riscv64.inline.hpp"

#ifndef PRODUCT
#define COMMENT(x)   do { __ block_comment(x); } while (0)
#else
#define COMMENT(x)
#endif

NEEDS_CLEANUP // remove this definitions ?
const Register IC_Klass    = t1;    // where the IC klass is cached
const Register SYNC_header = x10;   // synchronization header
const Register SHIFT_count = x10;   // where count for shift operations must be

#define __ _masm->

static void select_different_registers(Register preserve,
                                       Register extra,
                                       Register &tmp1,
                                       Register &tmp2) {
  if (tmp1 == preserve) {
    assert_different_registers(tmp1, tmp2, extra);
    tmp1 = extra;
  } else if (tmp2 == preserve) {
    assert_different_registers(tmp1, tmp2, extra);
    tmp2 = extra;
  }
  assert_different_registers(preserve, tmp1, tmp2);
}

static void select_different_registers(Register preserve,
                                       Register extra,
                                       Register &tmp1,
                                       Register &tmp2,
                                       Register &tmp3) {
  if (tmp1 == preserve) {
    assert_different_registers(tmp1, tmp2, tmp3, extra);
    tmp1 = extra;
  } else if (tmp2 == preserve) {
    assert_different_registers(tmp1, tmp2, tmp3, extra);
    tmp2 = extra;
  } else if (tmp3 == preserve) {
    assert_different_registers(tmp1, tmp2, tmp3, extra);
    tmp3 = extra;
  }
  assert_different_registers(preserve, tmp1, tmp2, tmp3);
}

bool LIR_Assembler::is_small_constant(LIR_Opr opr) { Unimplemented(); return false; }


LIR_Opr LIR_Assembler::receiverOpr() {
  return FrameMap::receiver_opr;
}

LIR_Opr LIR_Assembler::osrBufferPointer() {
  return FrameMap::as_pointer_opr(receiverOpr()->as_register());
}

//--------------fpu register translations-----------------------
void LIR_Assembler::set_24bit_FPU() { Unimplemented(); }

void LIR_Assembler::reset_FPU() { Unimplemented(); }

void LIR_Assembler::fpop() { Unimplemented(); }

void LIR_Assembler::fxch(int i) { Unimplemented(); }

void LIR_Assembler::fld(int i) { Unimplemented(); }

void LIR_Assembler::ffree(int i) { Unimplemented(); }

void LIR_Assembler::breakpoint() { Unimplemented(); }

void LIR_Assembler::push(LIR_Opr opr) { Unimplemented(); }

void LIR_Assembler::pop(LIR_Opr opr) { Unimplemented(); }
//-------------------------------------------

static jlong as_long(LIR_Opr data) {
  jlong result;
  switch (data->type()) {
    case T_INT:
      result = (data->as_jint());
      break;
    case T_LONG:
      result = (data->as_jlong());
      break;
    default:
      ShouldNotReachHere();
      result = 0;  // unreachable
  }
  return result;
}

Address LIR_Assembler::as_Address_hi(LIR_Address* addr) {
  ShouldNotReachHere();
  return Address();
}

Address LIR_Assembler::as_Address(LIR_Address* addr) {
  return as_Address(addr, t0);
}

Address LIR_Assembler::as_Address_lo(LIR_Address* addr) {
  return as_Address(addr);
}

void LIR_Assembler::osr_entry() {
  offsets()->set_value(CodeOffsets::OSR_Entry, code_offset());
  BlockBegin* osr_entry = compilation()->hir()->osr_entry();
  ValueStack* entry_state = osr_entry->state();
  int number_of_locks = entry_state->locks_size();

  // we jump here if osr happens with the interpreter
  // state set up to continue at the beginning of the
  // loop that triggered osr - in particular, we have
  // the following registers setup:
  //
  // x12: osr buffer
  //

  //build frame
  ciMethod* m = compilation()->method();
  __ build_frame(initial_frame_size_in_bytes(), bang_size_in_bytes());

  // OSR buffer is
  //
  // locals[nlocals-1..0]
  // monitors[0..number_of_locks]
  //
  // locals is a direct copy of the interpreter frame so in the osr buffer
  // so first slot in the local array is the last local from the interpreter
  // and last slot is local[0] (receiver) from the interpreter
  //
  // Similarly with locks. The first lock slot in the osr buffer is the nth lock
  // from the interpreter frame, the nth lock slot in the osr buffer is 0th lock
  // in the interpreter frame (the method lock if a sync method)

  // Initialize monitors in the compiled activation.
  //   x12: pointer to osr buffer
  // All other registers are dead at this point and the locals will be
  // copied into place by code emitted in the IR.

  Register OSR_buf = osrBufferPointer()->as_pointer_register();
  {
    assert(frame::interpreter_frame_monitor_size() == BasicObjectLock::size(), "adjust code below");
    int monitor_offset = BytesPerWord * method()->max_locals() +
      (2 * BytesPerWord) * (number_of_locks - 1);
    // SharedRuntime::OSR_migration_begin() packs BasicObjectLocks in
    // the OSR buffer using 2 word entries: first the lock and then
    // the oop.
    for (int i = 0; i < number_of_locks; i++) {
      int slot_offset = monitor_offset - ((i * 2) * BytesPerWord);
#ifdef ASSERT
      // verify the interpreter's monitor has a non-null object
      {
        Label L;
        __ ld(t0, Address(OSR_buf, slot_offset + 1 * BytesPerWord));
        __ bnez(t0, L);
        __ stop("locked object is NULL");
        __ bind(L);
      }
#endif // ASSERT
      __ ld(x9, Address(OSR_buf, slot_offset + 0));
      __ sd(x9, frame_map()->address_for_monitor_lock(i));
      __ ld(x9, Address(OSR_buf, slot_offset + 1 * BytesPerWord));
      __ sd(x9, frame_map()->address_for_monitor_object(i));
    }
  }
}

// inline cache check; done before the frame is built.
int LIR_Assembler::check_icache() {
  Register receiver = FrameMap::receiver_opr->as_register();
  Register ic_klass = IC_Klass;
  int start_offset = __ offset();
  Label dont;
  __ inline_cache_check(receiver, ic_klass, dont);

  // if icache check fails, then jump to runtime routine
  // Note: RECEIVER must still contain the receiver!
  __ far_jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));

  // We align the verified entry point unless the method body
  // (including its inline cache check) will fit in a single 64-byte
  // icache line.
  if (!method()->is_accessor() || __ offset() - start_offset > 4 * 4) {
    // force alignment after the cache check.
    __ align(CodeEntryAlignment);
  }

  __ bind(dont);
  return start_offset;
}

void LIR_Assembler::jobject2reg(jobject o, Register reg) {
  if (o == NULL) {
    __ mv(reg, zr);
  } else {
    __ movoop(reg, o, /*immediate*/ true);
  }
}

void LIR_Assembler::jobject2reg_with_patching(Register reg, CodeEmitInfo *info) {
  deoptimize_trap(info);
}

// This specifies the rsp decrement needed to build the frame
int LIR_Assembler::initial_frame_size_in_bytes() const {
  // if rounding, must let FrameMap know!

  // The frame_map records size in slots (32bit word)

  // subtract two words to account for return address and link
  return (frame_map()->framesize() - (2 * VMRegImpl::slots_per_word)) * VMRegImpl::stack_slot_size;
}

int LIR_Assembler::emit_exception_handler() {
  // if the last instruction is a call (typically to do a throw which
  // is coming at the end after block reordering) the return address
  // must still point into the code area in order to avoid assertion
  // failures when searching for the corresponding bci ==> add a nop
  // (was bug 5/14/1999 -gri)
  __ nop();

  // generate code for exception handler
  address handler_base = __ start_a_stub(exception_handler_size());
  if (handler_base == NULL) {
    // not enough space left for the handler
    bailout("exception handler overflow");
    return -1;
  }

  int offset = code_offset();

  // the exception oop and pc are in x10, and x13
  // no other registers need to be preserved, so invalidate them
  __ invalidate_registers(false, true, true, false, true, true);

  // check that there is really an exception
  __ verify_not_null_oop(x10);

  // search an exception handler (x10: exception oop, x13: throwing pc)
  __ far_call(RuntimeAddress(Runtime1::entry_for(Runtime1::handle_exception_from_callee_id)));
  __ should_not_reach_here();
  guarantee(code_offset() - offset <= exception_handler_size(), "overflow");
  __ end_a_stub();

  return offset;
}

// Emit the code to remove the frame from the stack in the exception
// unwind path.
int LIR_Assembler::emit_unwind_handler() {
#ifndef PRODUCT
  if (CommentedAssembly) {
    _masm->block_comment("Unwind handler");
  }
#endif // PRODUCT

  int offset = code_offset();

  // Fetch the exception from TLS and clear out exception related thread state
  __ ld(x10, Address(xthread, JavaThread::exception_oop_offset()));
  __ sd(zr, Address(xthread, JavaThread::exception_oop_offset()));
  __ sd(zr, Address(xthread, JavaThread::exception_pc_offset()));

  __ bind(_unwind_handler_entry);
  __ verify_not_null_oop(x10);
  if (method()->is_synchronized() || compilation()->env()->dtrace_method_probes()) {
    __ mv(x9, x10);   // Perserve the exception
  }

  // Preform needed unlocking
  MonitorExitStub* stub = NULL;
  if (method()->is_synchronized()) {
    monitor_address(0, FrameMap::r10_opr);
    stub = new MonitorExitStub(FrameMap::r10_opr, true, 0);
    __ unlock_object(x15, x14, x10, *stub->entry());
    __ bind(*stub->continuation());
  }

  if (compilation()->env()->dtrace_method_probes()) {
    __ call_Unimplemented();
  }

  if (method()->is_synchronized() || compilation()->env()->dtrace_method_probes()) {
    __ mv(x10, x9);   // Restore the exception
  }

  // remove the activation and dispatch to the unwind handler
  __ block_comment("remove_frame and dispatch to the unwind handler");
  __ remove_frame(initial_frame_size_in_bytes());
  __ far_jump(RuntimeAddress(Runtime1::entry_for(Runtime1::unwind_exception_id)));

  // Emit the slow path assembly
  if (stub != NULL) {
    stub->emit_code(this);
  }

  return offset;
}

int LIR_Assembler::emit_deopt_handler() {
  // if the last instruciton is a call (typically to do a throw which
  // is coming at the end after block reordering) the return address
  // must still point into the code area in order to avoid assertion
  // failures when searching for the corresponding bck => add a nop
  // (was bug 5/14/1999 - gri)
  __ nop();

  // generate code for exception handler
  address handler_base = __ start_a_stub(deopt_handler_size());
  if (handler_base == NULL) {
    // not enough space left for the handler
    bailout("deopt handler overflow");
    return -1;
  }

  int offset = code_offset();

  __ auipc(ra, 0);
  __ far_jump(RuntimeAddress(SharedRuntime::deopt_blob()->unpack()));
  guarantee(code_offset() - offset <= deopt_handler_size(), "overflow");
  __ end_a_stub();

  return offset;
}

void LIR_Assembler::return_op(LIR_Opr result) {
  assert(result->is_illegal() || !result->is_single_cpu() || result->as_register() == x10, "word returns are in x10");

  // Pop the stack before the safepoint code
  __ remove_frame(initial_frame_size_in_bytes());

  if (StackReservedPages > 0 && compilation()->has_reserved_stack_access()) {
    __ reserved_stack_check();
  }

  address polling_page(os::get_polling_page());
  __ read_polling_page(t0, polling_page, relocInfo::poll_return_type);
  __ ret();
}

int LIR_Assembler::safepoint_poll(LIR_Opr tmp, CodeEmitInfo* info) {
  address polling_page(os::get_polling_page());
  guarantee(info != NULL, "Shouldn't be NULL");
  assert(os::is_poll_address(polling_page), "should be");
  int32_t offset = 0;
  __ get_polling_page(t0, polling_page, offset, relocInfo::poll_type);
  add_debug_info_for_branch(info);  // This isn't just debug info:
                                    // it's the oop map
  __ read_polling_page(t0, offset, relocInfo::poll_type);
  return __ offset();
}

void LIR_Assembler::move_regs(Register from_reg, Register to_reg) {
  __ mv(to_reg, from_reg);
}

void LIR_Assembler::swap_reg(Register a, Register b) { Unimplemented(); }

void LIR_Assembler::const2reg(LIR_Opr src, LIR_Opr dest, LIR_PatchCode patch_code, CodeEmitInfo* info) {
  assert(src->is_constant(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");
  LIR_Const* c = src->as_constant_ptr();
  address const_addr = NULL;

  switch (c->type()) {
    case T_INT:
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mvw(dest->as_register(), c->as_jint());
      break;

    case T_ADDRESS:
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mv(dest->as_register(), c->as_jint());
      break;

    case T_LONG:
      assert(patch_code == lir_patch_none, "no patching handled here");
      __ mv(dest->as_register_lo(), (intptr_t)c->as_jlong());
      break;

    case T_OBJECT:
    case T_ARRAY:
      if (patch_code == lir_patch_none) {
        jobject2reg(c->as_jobject(), dest->as_register());
      } else {
        jobject2reg_with_patching(dest->as_register(), info);
      }
      break;

    case T_METADATA:
      if (patch_code != lir_patch_none) {
        klass2reg_with_patching(dest->as_register(), info);
      } else {
        __ mov_metadata(dest->as_register(), c->as_metadata());
      }
      break;

    case T_FLOAT:
      const_addr = float_constant(c->as_jfloat());
      assert(const_addr != NULL, "must create float constant in the constant table");
      __ flw(dest->as_float_reg(), InternalAddress(const_addr));
      break;

    case T_DOUBLE:
      const_addr = double_constant(c->as_jdouble());
      assert(const_addr != NULL, "must create double constant in the constant table");
      __ fld(dest->as_double_reg(), InternalAddress(const_addr));
      break;

    default:
      ShouldNotReachHere();
  }
}

void LIR_Assembler::const2stack(LIR_Opr src, LIR_Opr dest) {
  assert(src->is_constant(), "should not call otherwise");
  assert(dest->is_stack(), "should not call otherwise");
  LIR_Const* c = src->as_constant_ptr();
  switch (c->type()) {
    case T_OBJECT:
      if (c->as_jobject() == NULL) {
        __ sd(zr, frame_map()->address_for_slot(dest->single_stack_ix()));
      } else {
        const2reg(src, FrameMap::t1_opr, lir_patch_none, NULL);
        reg2stack(FrameMap::t1_opr, dest, c->type(), false);
      }
      break;
    case T_ADDRESS:   // fall through
      const2reg(src, FrameMap::t1_opr, lir_patch_none, NULL);
      reg2stack(FrameMap::t1_opr, dest, c->type(), false);
    case T_INT:       // fall through
    case T_FLOAT:
      if (c->as_jint_bits() == 0) {
        __ sw(zr, frame_map()->address_for_slot(dest->single_stack_ix()));
      } else {
        __ mvw(t1, c->as_jint_bits());
        __ sw(t1, frame_map()->address_for_slot(dest->single_stack_ix()));
      }
      break;
    case T_LONG:      // fall through
    case T_DOUBLE:
      if (c->as_jlong_bits() == 0) {
        __ sd(zr, frame_map()->address_for_slot(dest->double_stack_ix(),
                                                lo_word_offset_in_bytes));
      } else {
        __ mv(t1, (intptr_t)c->as_jlong_bits());
        __ sd(t1, frame_map()->address_for_slot(dest->double_stack_ix(),
                                                lo_word_offset_in_bytes));
      }
      break;
    default:
      ShouldNotReachHere();
  }
}

void LIR_Assembler::const2mem(LIR_Opr src, LIR_Opr dest, BasicType type, CodeEmitInfo* info, bool wide) {
  assert(src->is_constant(), "should not call otherwise");
  assert(dest->is_address(), "should not call otherwise");
  LIR_Const* c = src->as_constant_ptr();
  LIR_Address* to_addr = dest->as_address_ptr();
  void (Assembler::* insn)(Register Rt, const Address &adr, Register temp);
  switch (type) {
    case T_ADDRESS:
      assert(c->as_jint() == 0, "should be");
      insn = &Assembler::sd; break;
    case T_LONG:
      assert(c->as_jlong() == 0, "should be");
      insn = &Assembler::sd; break;
    case T_DOUBLE:
      assert(c->as_jdouble() == 0.0, "should be");
      insn = &Assembler::sd; break;
    case T_INT:
      assert(c->as_jint() == 0, "should be");
      insn = &Assembler::sw; break;
    case T_FLOAT:
      assert(c->as_jfloat() == 0.0f, "should be");
      insn = &Assembler::sw; break;
    case T_OBJECT:    // fall through
    case T_ARRAY:
      assert(c->as_jobject() == 0, "should be");
      if (UseCompressedOops && !wide) {
        insn = &Assembler::sw;
      } else {
        insn = &Assembler::sd;
      }
      break;
    case T_CHAR:      // fall through
    case T_SHORT:
      assert(c->as_jint() == 0, "should be");
      insn = &Assembler::sh;
      break;
    case T_BOOLEAN:   // fall through
    case T_BYTE:
      assert(c->as_jint() == 0, "should be");
      insn = &Assembler::sb; break;
    default:
      ShouldNotReachHere();
      insn = &Assembler::sd;  // unreachable
  }
  if (info != NULL) {
    add_debug_info_for_null_check_here(info);
  }
  (_masm->*insn)(zr, as_Address(to_addr), t0);
}

void LIR_Assembler::reg2reg(LIR_Opr src, LIR_Opr dest) {
  assert(src->is_register(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");

  // move between cpu-registers
  if (dest->is_single_cpu()) {
    if (src->type() == T_LONG) {
      // Can do LONG -> OBJECT
      move_regs(src->as_register_lo(), dest->as_register());
      return;
    }
    assert(src->is_single_cpu(), "must match");
    if (src->type() == T_OBJECT) {
      __ verify_oop(src->as_register());
    }
    move_regs(src->as_register(), dest->as_register());
  } else if (dest->is_double_cpu()) {
    if (src->type() == T_OBJECT || src->type() == T_ARRAY) {
      __ verify_oop(src->as_register());
      move_regs(src->as_register(), dest->as_register_lo());
      return;
    }
    assert(src->is_double_cpu(), "must match");
    Register f_lo = src->as_register_lo();
    Register f_hi = src->as_register_hi();
    Register t_lo = dest->as_register_lo();
    Register t_hi = dest->as_register_hi();
    assert(f_hi == f_lo, "must be same");
    assert(t_hi == t_lo, "must be same");
    move_regs(f_lo, t_lo);
  } else if (dest->is_single_fpu()) {
    assert(src->is_single_fpu(), "expect single fpu");
    __ fmv_s(dest->as_float_reg(), src->as_float_reg());
  } else if (dest->is_double_fpu()) {
    assert(src->is_double_fpu(), "expect double fpu");
    __ fmv_d(dest->as_double_reg(), src->as_double_reg());
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::reg2stack(LIR_Opr src, LIR_Opr dest, BasicType type, bool pop_fpu_stack) {
  assert(src->is_register(), "should not call otherwise");
  assert(dest->is_stack(), "should not call otherwise");
  if (src->is_single_cpu()) {
    if (type == T_ARRAY || type == T_OBJECT) {
      __ sd(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
      __ verify_oop(src->as_register());
    } else if (type == T_METADATA || type == T_DOUBLE || type == T_ADDRESS) {
      __ sd(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
    } else {
      __ sw(src->as_register(), frame_map()->address_for_slot(dest->single_stack_ix()));
    }
  } else if (src->is_double_cpu()) {
    Address dest_addr_LO = frame_map()->address_for_slot(dest->double_stack_ix(), lo_word_offset_in_bytes);
    __ sd(src->as_register_lo(), dest_addr_LO);
  } else if (src->is_single_fpu()) {
    Address dest_addr = frame_map()->address_for_slot(dest->single_stack_ix());
    __ fsw(src->as_float_reg(), dest_addr);
  } else if (src->is_double_fpu()) {
    Address dest_addr = frame_map()->address_for_slot(dest->double_stack_ix());
    __ fsd(src->as_double_reg(), dest_addr);
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::reg2mem(LIR_Opr src, LIR_Opr dest, BasicType type, LIR_PatchCode patch_code, CodeEmitInfo* info,
                            bool pop_fpu_stack, bool wide, bool /* unaligned */) {
  LIR_Address* to_addr = dest->as_address_ptr();
  // t0 was used as tmp reg in as_Address, so we use t1 as compressed_src
  Register compressed_src = t1;

  if (patch_code != lir_patch_none) {
    deoptimize_trap(info);
    return;
  }

  if (type == T_ARRAY || type == T_OBJECT) {
    __ verify_oop(src->as_register());

    if (UseCompressedOops && !wide) {
      __ encode_heap_oop(compressed_src, src->as_register());
    } else {
      compressed_src = src->as_register();
    }
  }

  int null_check_here = code_offset();

  switch (type) {
    case T_FLOAT:
      __ fsw(src->as_float_reg(), as_Address(to_addr));
      break;

    case T_DOUBLE:
      __ fsd(src->as_double_reg(), as_Address(to_addr));
      break;

    case T_ARRAY:      // fall through
    case T_OBJECT:
      if (UseCompressedOops && !wide) {
        __ sw(compressed_src, as_Address(to_addr));
      } else {
        __ sd(compressed_src, as_Address(to_addr));
      }
      break;
    case T_METADATA:
      // We get here to store a method pointer to the stack to pass to
      // a dtrace runtime call. This can't work on 64 bit with
      // compressed klass ptrs: T_METADATA can be compressed klass
      // ptr or a 64 bit method pointer.
      ShouldNotReachHere();
      __ sd(src->as_register(), as_Address(to_addr));
      break;
    case T_ADDRESS:
      __ sd(src->as_register(), as_Address(to_addr));
      break;
    case T_INT:
      __ sw(src->as_register(), as_Address(to_addr));
      break;
    case T_LONG:
      __ sd(src->as_register_lo(), as_Address(to_addr));
      break;
    case T_BYTE:    // fall through
    case T_BOOLEAN:
      __ sb(src->as_register(), as_Address(to_addr));
      break;
    case T_CHAR:    // fall through
    case T_SHORT:
      __ sh(src->as_register(), as_Address(to_addr));
      break;
    default:
      ShouldNotReachHere();
  }

  if (info != NULL) {
    add_debug_info_for_null_check(null_check_here, info);
  }
}

void LIR_Assembler::stack2reg(LIR_Opr src, LIR_Opr dest, BasicType type) {
  assert(src->is_stack(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");

  if (dest->is_single_cpu()) {
    if (type == T_INT) {
      __ lw(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
    } else if (type == T_ARRAY || type == T_OBJECT) {
      __ ld(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
      __ verify_oop(dest->as_register());
    } else if (type == T_METADATA || type == T_ADDRESS) {
      __ ld(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
    } else {
      __ lwu(dest->as_register(), frame_map()->address_for_slot(src->single_stack_ix()));
    }
  } else if (dest->is_double_cpu()) {
    Address src_addr_LO = frame_map()->address_for_slot(src->double_stack_ix(), lo_word_offset_in_bytes);
    __ ld(dest->as_register_lo(), src_addr_LO);
  } else if (dest->is_single_fpu()) {
    Address src_addr = frame_map()->address_for_slot(src->single_stack_ix());
    __ flw(dest->as_float_reg(), src_addr);
  } else if (dest->is_double_fpu()) {
    Address src_addr = frame_map()->address_for_slot(src->double_stack_ix());
    __ fld(dest->as_double_reg(), src_addr);
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::klass2reg_with_patching(Register reg, CodeEmitInfo* info) {
  address target = NULL;
  relocInfo::relocType reloc_type = relocInfo::none;

  switch (patching_id(info)) {
    case PatchingStub::access_field_id:
      target = Runtime1::entry_for(Runtime1::access_field_patching_id);
      reloc_type = relocInfo::section_word_type;
      break;
    case PatchingStub::load_klass_id:
      target = Runtime1::entry_for(Runtime1::load_klass_patching_id);
      reloc_type = relocInfo::metadata_type;
      break;
    case PatchingStub::load_mirror_id:
      target = Runtime1::entry_for(Runtime1::load_mirror_patching_id);
      reloc_type = relocInfo::oop_type;
      break;
    case PatchingStub::load_appendix_id:
      target = Runtime1::entry_for(Runtime1::load_appendix_patching_id);
      reloc_type = relocInfo::oop_type;
      break;
    default:
      ShouldNotReachHere();
  }

  __ far_call(RuntimeAddress(target));
  add_call_info_here(info);
}

void LIR_Assembler::stack2stack(LIR_Opr src, LIR_Opr dest, BasicType type) {
  LIR_Opr temp;
  if (type == T_LONG || type == T_DOUBLE) {
    temp = FrameMap::t1_long_opr;
  } else {
    temp = FrameMap::t1_opr;
  }

  stack2reg(src, temp, src->type());
  reg2stack(temp, dest, dest->type(), false);
}

void LIR_Assembler::mem2reg(LIR_Opr src, LIR_Opr dest, BasicType type, LIR_PatchCode patch_code, CodeEmitInfo* info,
                            bool wide, bool /* unaligned */) {
  assert(src->is_address(), "should not call otherwise");
  assert(dest->is_register(), "should not call otherwise");

  LIR_Address* addr = src->as_address_ptr();
  LIR_Address* from_addr = src->as_address_ptr();

  if (addr->base()->type() == T_OBJECT) {
    __ verify_oop(addr->base()->as_pointer_register());
  }

  if (patch_code != lir_patch_none) {
    deoptimize_trap(info);
    return;
  }

  if (info != NULL) {
    add_debug_info_for_null_check_here(info);
  }

  int null_check_here = code_offset();
  switch (type) {
    case T_FLOAT:
      __ flw(dest->as_float_reg(), as_Address(from_addr));
      break;
    case T_DOUBLE:
      __ fld(dest->as_double_reg(), as_Address(from_addr));
      break;
    case T_ARRAY:     // fall through
    case T_OBJECT:
      if (UseCompressedOops && !wide) {
        __ lwu(dest->as_register(), as_Address(from_addr));
      } else {
        __ ld(dest->as_register(), as_Address(from_addr));
      }
      break;
    case T_METADATA:
      // We get here to store a method pointer to the stack to pass to
      // a dtrace runtime call. This can't work on 64 bit with
      // compressed klass ptrs: T_METADATA can be a compressed klass
      // ptr or a 64 bit method pointer.
      ShouldNotReachHere();
      __ ld(dest->as_register(), as_Address(from_addr));
      break;
    case T_ADDRESS:
      if (UseCompressedClassPointers && addr->disp() == oopDesc::klass_offset_in_bytes()) {
        __ lwu(dest->as_register(), as_Address(from_addr));
      } else {
        __ ld(dest->as_register(), as_Address(from_addr));
      }
      break;
    case T_INT:
      __ lw(dest->as_register(), as_Address(from_addr));
      break;
    case T_LONG:
      __ ld(dest->as_register_lo(), as_Address_lo(from_addr));
      break;
    case T_BYTE:
      __ lb(dest->as_register(), as_Address(from_addr));
      break;
    case T_BOOLEAN:
      __ lbu(dest->as_register(), as_Address(from_addr));
      break;
    case T_CHAR:
      __ lhu(dest->as_register(), as_Address(from_addr));
      break;
    case T_SHORT:
      __ lh(dest->as_register(), as_Address(from_addr));
      break;
    default:
      ShouldNotReachHere();
  }

  if (type == T_ARRAY || type == T_OBJECT) {
    if (UseCompressedOops && !wide) {
      __ decode_heap_oop(dest->as_register());
    }
    __ verify_oop(dest->as_register());
  } else if (type == T_ADDRESS && addr->disp() == oopDesc::klass_offset_in_bytes()) {
    if (UseCompressedClassPointers) {
      __ decode_klass_not_null(dest->as_register());
    }
  }
}

void LIR_Assembler::emit_op3(LIR_Op3* op) {
  switch (op->code()) {
    case lir_idiv:
      arithmetic_idiv(op, false);
      break;
    case lir_irem:
      arithmetic_idiv(op, true);
      break;
    case lir_fmad:
      __ fmadd_d(op->result_opr()->as_double_reg(),
                 op->in_opr1()->as_double_reg(),
                 op->in_opr2()->as_double_reg(),
                 op->in_opr3()->as_double_reg());
      break;
    case lir_fmaf:
      __ fmadd_s(op->result_opr()->as_float_reg(),
                 op->in_opr1()->as_float_reg(),
                 op->in_opr2()->as_float_reg(),
                 op->in_opr3()->as_float_reg());
      break;
    default:
      ShouldNotReachHere();
      break;
  }
}

void LIR_Assembler::emit_op4(LIR_Op4* op) {
 switch (op->code()) {
    case lir_cmove:
      emit_cmove(op);
      break;
    default:      ShouldNotReachHere(); break;
  }
}

void LIR_Assembler::emit_cmove(LIR_Op4* op) {
  LIR_Opr cmp1 = op->in_opr1();
  LIR_Opr cmp2 = op->in_opr2();
  LIR_Opr src1 = op->in_opr3();
  LIR_Opr src2 = op->in_opr4();
  LIR_Condition condition = op->cond();
  LIR_Opr dst = op->result_opr();
  Label label;

  emit_branch(condition, cmp1, cmp2, label, /* is_far */ false,
              /* is_unordered */ (condition == lir_cond_greaterEqual || condition == lir_cond_greater) ? false : true);

  Label done;
  move_op(src2, dst, op->type(), lir_patch_none, NULL,
          false,   // pop_fpu_stack
          false,   // unaligned
          false);  // wide
  __ j(done);
  __ bind(label);
  move_op(src1, dst, op->type(), lir_patch_none, NULL,
          false,   // pop_fpu_stack
          false,   // unaligned
          false);  // wide
  __ bind(done);
}

void LIR_Assembler::emit_opBranch(LIR_OpBranch* op) {
  LIR_Condition condition = op->cond();
  if (condition == lir_cond_always && op->info() != NULL) { add_debug_info_for_branch(op->info()); }
  bool is_unordered = (op->ublock() == op->block());
  emit_branch(condition, op->left(), op->right(), *op->label(), /* is_far */ true, is_unordered);
}

void LIR_Assembler::emit_branch(LIR_Condition cmp_flag, LIR_Opr cmp1, LIR_Opr cmp2, Label& label,
                                bool is_far, bool is_unordered) {

  if (cmp_flag == lir_cond_always) {
    __ j(label);
    return;
  }

  if (cmp1->is_cpu_register()) {
    Register reg1 = as_reg(cmp1);
    if (cmp2->is_cpu_register()) {
      Register reg2 = as_reg(cmp2);
      __ c1_cmp_branch(cmp_flag, reg1, reg2, label, cmp1->type(), is_far);
    } else if (cmp2->is_constant()) {
      const2reg_helper(cmp2);
      __ c1_cmp_branch(cmp_flag, reg1, t0, label, cmp2->type(), is_far);
    } else {
      ShouldNotReachHere();
    }
  } else if (cmp1->is_single_fpu()) {
    assert(cmp2->is_single_fpu(), "expect single float register");
    __ c1_float_cmp_branch(cmp_flag, cmp1->as_float_reg(), cmp2->as_float_reg(), label, is_far, is_unordered);
  } else if (cmp1->is_double_fpu()) {
    assert(cmp2->is_double_fpu(), "expect double float register");
    __ c1_float_cmp_branch(cmp_flag | C1_MacroAssembler::c1_double_branch_mask,
                           cmp1->as_double_reg(), cmp2->as_double_reg(), label, is_far, is_unordered);
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::emit_opConvert(LIR_OpConvert* op) {
  LIR_Opr src  = op->in_opr();
  LIR_Opr dest = op->result_opr();

  switch (op->bytecode()) {
    case Bytecodes::_i2f:
      __ fcvt_s_w(dest->as_float_reg(), src->as_register()); break;
    case Bytecodes::_i2d:
      __ fcvt_d_w(dest->as_double_reg(), src->as_register()); break;
    case Bytecodes::_l2d:
      __ fcvt_d_l(dest->as_double_reg(), src->as_register_lo()); break;
    case Bytecodes::_l2f:
      __ fcvt_s_l(dest->as_float_reg(), src->as_register_lo()); break;
    case Bytecodes::_f2d:
      __ fcvt_d_s(dest->as_double_reg(), src->as_float_reg()); break;
    case Bytecodes::_d2f:
      __ fcvt_s_d(dest->as_float_reg(), src->as_double_reg()); break;
    case Bytecodes::_i2c:
      __ zero_ext(dest->as_register(), src->as_register(), registerSize - 16); break; // 16: char size
    case Bytecodes::_i2l:
      __ addw(dest->as_register_lo(), src->as_register(), zr); break;
    case Bytecodes::_i2s:
      __ sign_ext(dest->as_register(), src->as_register(), registerSize - 16); break; // 16: short size
    case Bytecodes::_i2b:
      __ sign_ext(dest->as_register(), src->as_register(), registerSize - 8); break;  // 8: byte size
    case Bytecodes::_l2i:
      _masm->block_comment("FIXME: This coulde be no-op");
      __ addw(dest->as_register(), src->as_register_lo(), zr); break;
    case Bytecodes::_d2l:
      __ fcvt_l_d_safe(dest->as_register_lo(), src->as_double_reg()); break;
    case Bytecodes::_f2i:
      __ fcvt_w_s_safe(dest->as_register(), src->as_float_reg()); break;
    case Bytecodes::_f2l:
      __ fcvt_l_s_safe(dest->as_register_lo(), src->as_float_reg()); break;
    case Bytecodes::_d2i:
      __ fcvt_w_d_safe(dest->as_register(), src->as_double_reg()); break;
    default:
      ShouldNotReachHere();
  }
}

void LIR_Assembler::emit_alloc_obj(LIR_OpAllocObj* op) {
  if (op->init_check()) {
    __ lbu(t0, Address(op->klass()->as_register(),
                       InstanceKlass::init_state_offset()));
    __ mvw(t1, InstanceKlass::fully_initialized);
    add_debug_info_for_null_check_here(op->stub()->info());
    __ bne(t0, t1, *op->stub()->entry(), /* is_far */ true);
  }

  __ allocate_object(op->obj()->as_register(),
                     op->tmp1()->as_register(),
                     op->tmp2()->as_register(),
                     op->header_size(),
                     op->object_size(),
                     op->klass()->as_register(),
                     *op->stub()->entry());

  __ bind(*op->stub()->continuation());
}

void LIR_Assembler::emit_alloc_array(LIR_OpAllocArray* op) {
  Register len = op->len()->as_register();

  if (UseSlowPath ||
      (!UseFastNewObjectArray && (op->type() == T_OBJECT || op->type() == T_ARRAY)) ||
      (!UseFastNewTypeArray   && (op->type() != T_OBJECT && op->type() != T_ARRAY))) {
    __ j(*op->stub()->entry());
  } else {
    Register tmp1 = op->tmp1()->as_register();
    Register tmp2 = op->tmp2()->as_register();
    Register tmp3 = op->tmp3()->as_register();
    if (len == tmp1) {
      tmp1 = tmp3;
    } else if (len == tmp2) {
      tmp2 = tmp3;
    } else if (len == tmp3) {
      // everything is ok
    } else {
      __ mv(tmp3, len);
    }
    __ allocate_array(op->obj()->as_register(),
                      len,
                      tmp1,
                      tmp2,
                      arrayOopDesc::header_size(op->type()),
                      array_element_size(op->type()),
                      op->klass()->as_register(),
                      *op->stub()->entry());
  }
  __ bind(*op->stub()->continuation());
}

void LIR_Assembler::type_profile_helper(Register mdo, ciMethodData *md, ciProfileData *data,
                                        Register recv, Label* update_done) {
  for (uint i = 0; i < ReceiverTypeData::row_limit(); i++) {
    Label next_test;
    // See if the receiver is receiver[n].
    __ ld(t1, Address(mdo, md->byte_offset_of_slot(data, ReceiverTypeData::receiver_offset(i))));
    __ bne(recv, t1, next_test);
    Address data_addr(mdo, md->byte_offset_of_slot(data, ReceiverTypeData::receiver_count_offset(i)));
    __ add_memory_int64(data_addr, DataLayout::counter_increment);
    __ j(*update_done);
    __ bind(next_test);
  }

  // Didn't find receiver; find next empty slot and fill it in
  for (uint i = 0; i < ReceiverTypeData::row_limit(); i++) {
    Label next_test;
    Address recv_addr(mdo, md->byte_offset_of_slot(data, ReceiverTypeData::receiver_offset(i)));
    __ ld(t1, recv_addr);
    __ bnez(t1, next_test);
    __ sd(recv, recv_addr);
    __ li(t1, DataLayout::counter_increment);
    __ sd(t1, Address(mdo, md->byte_offset_of_slot(data, ReceiverTypeData::receiver_count_offset(i))));
    __ j(*update_done);
    __ bind(next_test);
  }
}

void LIR_Assembler::data_check(LIR_OpTypeCheck *op, ciMethodData **md, ciProfileData **data) {
  ciMethod* method = op->profiled_method();
  assert(method != NULL, "Should have method");
  int bci = op->profiled_bci();
  *md = method->method_data_or_null();
  assert(*md != NULL, "Sanity");
  *data = ((*md)->bci_to_data(bci));
  assert(*data != NULL, "need data for type check");
  assert((*data)->is_ReceiverTypeData(), "need ReceiverTypeData for type check");
}

void LIR_Assembler::typecheck_helper_slowcheck(ciKlass *k, Register obj, Register Rtmp1,
                                               Register k_RInfo, Register klass_RInfo,
                                               Label *failure_target, Label *success_target) {
  // get object class
  // not a safepoint as obj null check happens earlier
  __ load_klass(klass_RInfo, obj);
  if (k->is_loaded()) {
    // See if we get an immediate positive hit
    __ ld(t0, Address(klass_RInfo, long(k->super_check_offset())));
    if ((juint)in_bytes(Klass::secondary_super_cache_offset()) != k->super_check_offset()) {
      __ bne(k_RInfo, t0, *failure_target, /* is_far */ true);
      // successful cast, fall through to profile or jump
    } else {
      // See if we get an immediate positive hit
      __ beq(k_RInfo, t0, *success_target);
      // check for self
      __ beq(klass_RInfo, k_RInfo, *success_target);

      __ addi(sp, sp, -2 * wordSize); // 2: store k_RInfo and klass_RInfo
      __ sd(k_RInfo, Address(sp, 0));             // sub klass
      __ sd(klass_RInfo, Address(sp, wordSize));  // super klass
      __ far_call(RuntimeAddress(Runtime1::entry_for(Runtime1::slow_subtype_check_id)));
      // load result to k_RInfo
      __ ld(k_RInfo, Address(sp, 0));
      __ addi(sp, sp, 2 * wordSize); // 2: pop out k_RInfo and klass_RInfo
      // result is a boolean
      __ beqz(k_RInfo, *failure_target, /* is_far */ true);
      // successful cast, fall through to profile or jump
    }
  } else {
    // perform the fast part of the checking logic
    __ check_klass_subtype_fast_path(klass_RInfo, k_RInfo, Rtmp1, success_target, failure_target, NULL);
    // call out-of-line instance of __ check_klass_subtytpe_slow_path(...)
    __ addi(sp, sp, -2 * wordSize); // 2: store k_RInfo and klass_RInfo
    __ sd(klass_RInfo, Address(sp, wordSize));  // sub klass
    __ sd(k_RInfo, Address(sp, 0));             // super klass
    __ far_call(RuntimeAddress(Runtime1::entry_for(Runtime1::slow_subtype_check_id)));
    // load result to k_RInfo
    __ ld(k_RInfo, Address(sp, 0));
    __ addi(sp, sp, 2 * wordSize); // 2: pop out k_RInfo and klass_RInfo
    // result is a boolean
    __ beqz(k_RInfo, *failure_target, /* is_far */ true);
    // successful cast, fall thriugh to profile or jump
  }
}

void LIR_Assembler::profile_object(ciMethodData* md, ciProfileData* data, Register obj,
                                   Register klass_RInfo, Label* obj_is_null) {
  Label not_null;
  __ bnez(obj, not_null);
  // Object is null, update MDO and exit
  Register mdo = klass_RInfo;
  __ mov_metadata(mdo, md->constant_encoding());
  Address data_addr = __ form_address(t1, mdo, md->byte_offset_of_slot(data, DataLayout::flags_offset()));
  __ lbu(t0, data_addr);
  __ ori(t0, t0, BitData::null_seen_byte_constant());
  __ sb(t0, data_addr);
  __ j(*obj_is_null);
  __ bind(not_null);
}

void LIR_Assembler::typecheck_loaded(LIR_OpTypeCheck *op, ciKlass* k, Register k_RInfo) {
  if (!k->is_loaded()) {
    klass2reg_with_patching(k_RInfo, op->info_for_patch());
  } else {
    __ mov_metadata(k_RInfo, k->constant_encoding());
  }
}

void LIR_Assembler::emit_typecheck_helper(LIR_OpTypeCheck *op, Label* success, Label* failure, Label* obj_is_null) {
  Register obj = op->object()->as_register();
  Register k_RInfo = op->tmp1()->as_register();
  Register klass_RInfo = op->tmp2()->as_register();
  Register dst = op->result_opr()->as_register();
  ciKlass* k = op->klass();
  Register Rtmp1 = noreg;

  // check if it needs to be profiled
  ciMethodData* md = NULL;
  ciProfileData* data = NULL;

  const bool should_profile = op->should_profile();
  if (should_profile) {
    data_check(op, &md, &data);
  }
  Label profile_cast_success, profile_cast_failure;
  Label *success_target = should_profile ? &profile_cast_success : success;
  Label *failure_target = should_profile ? &profile_cast_failure : failure;

  if (obj == k_RInfo) {
    k_RInfo = dst;
  } else if (obj == klass_RInfo) {
    klass_RInfo = dst;
  }
  if (k->is_loaded() && !UseCompressedClassPointers) {
    select_different_registers(obj, dst, k_RInfo, klass_RInfo);
  } else {
    Rtmp1 = op->tmp3()->as_register();
    select_different_registers(obj, dst, k_RInfo, klass_RInfo, Rtmp1);
  }

  assert_different_registers(obj, k_RInfo, klass_RInfo);

  if (should_profile) {
    profile_object(md, data, obj, klass_RInfo, obj_is_null);
  } else {
    __ beqz(obj, *obj_is_null);
  }

  typecheck_loaded(op, k, k_RInfo);
  __ verify_oop(obj);

  if (op->fast_check()) {
    // get object class
    // not a safepoint as obj null check happens earlier
    __ load_klass(t0, obj);
    __ bne(t0, k_RInfo, *failure_target, /* is_far */ true);
    // successful cast, fall through to profile or jump
  } else {
    typecheck_helper_slowcheck(k, obj, Rtmp1, k_RInfo, klass_RInfo, failure_target, success_target);
  }
  if (should_profile) {
    type_profile(obj, md, klass_RInfo, k_RInfo, data, success, failure, profile_cast_success, profile_cast_failure);
  }
  __ j(*success);
}

void LIR_Assembler::emit_opTypeCheck(LIR_OpTypeCheck* op) {
  const bool should_profile = op->should_profile();

  LIR_Code code = op->code();
  if (code == lir_store_check) {
    typecheck_lir_store(op, should_profile);
  } else if (code == lir_checkcast) {
    Register obj = op->object()->as_register();
    Register dst = op->result_opr()->as_register();
    Label success;
    emit_typecheck_helper(op, &success, op->stub()->entry(), &success);
    __ bind(success);
    if (dst != obj) {
      __ mv(dst, obj);
    }
  } else if (code == lir_instanceof) {
    Register obj = op->object()->as_register();
    Register dst = op->result_opr()->as_register();
    Label success, failure, done;
    emit_typecheck_helper(op, &success, &failure, &failure);
    __ bind(failure);
    __ mv(dst, zr);
    __ j(done);
    __ bind(success);
    __ mv(dst, 1);
    __ bind(done);
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::emit_compare_and_swap(LIR_OpCompareAndSwap* op) {
  assert(VM_Version::supports_cx8(), "wrong machine");
  Register addr;
  if (op->addr()->is_register()) {
    addr = as_reg(op->addr());
  } else {
    assert(op->addr()->is_address(), "what else?");
    LIR_Address* addr_ptr = op->addr()->as_address_ptr();
    assert(addr_ptr->disp() == 0, "need 0 disp");
    assert(addr_ptr->index() == LIR_OprDesc::illegalOpr(), "need 0 index");
    addr = as_reg(addr_ptr->base());
  }
  Register newval = as_reg(op->new_value());
  Register cmpval = as_reg(op->cmp_value());

  if (op->code() == lir_cas_obj) {
    if (UseCompressedOops) {
      Register tmp1 = op->tmp1()->as_register();
      assert(op->tmp1()->is_valid(), "must be");
      __ encode_heap_oop(tmp1, cmpval);
      cmpval = tmp1;
      __ encode_heap_oop(t1, newval);
      newval = t1;
      caswu(addr, newval, cmpval);
    } else {
      casl(addr, newval, cmpval);
    }
  } else if (op->code() == lir_cas_int) {
    casw(addr, newval, cmpval);
  } else {
    casl(addr, newval, cmpval);
  }
}

void LIR_Assembler::intrinsic_op(LIR_Code code, LIR_Opr value, LIR_Opr unused, LIR_Opr dest, LIR_Op* op) {
  switch (code) {
    case lir_abs:  __ fabs_d(dest->as_double_reg(), value->as_double_reg()); break;
    case lir_sqrt: __ fsqrt_d(dest->as_double_reg(), value->as_double_reg()); break;
    default:       ShouldNotReachHere();
  }
}

void LIR_Assembler::logic_op(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr dst) {
  assert(left->is_single_cpu() || left->is_double_cpu(), "expect single or double register");
  Register Rleft = left->is_single_cpu() ? left->as_register() : left->as_register_lo();
  if (dst->is_single_cpu()) {
    Register Rdst = dst->as_register();
    if (right->is_constant()) {
      int right_const = right->as_jint();
      if (Assembler::operand_valid_for_add_immediate(right_const)) {
        logic_op_imm(Rdst, Rleft, right_const, code);
        __ addw(Rdst, Rdst, zr);
     } else {
        __ mv(t0, right_const);
        logic_op_reg32(Rdst, Rleft, t0, code);
     }
    } else {
      Register Rright = right->is_single_cpu() ? right->as_register() : right->as_register_lo();
      logic_op_reg32(Rdst, Rleft, Rright, code);
    }
  } else {
    Register Rdst = dst->as_register_lo();
    if (right->is_constant()) {
      long right_const = right->as_jlong();
      if (Assembler::operand_valid_for_add_immediate(right_const)) {
        logic_op_imm(Rdst, Rleft, right_const, code);
      } else {
        __ mv(t0, right_const);
        logic_op_reg(Rdst, Rleft, t0, code);
      }
    } else {
      Register Rright = right->is_single_cpu() ? right->as_register() : right->as_register_lo();
      logic_op_reg(Rdst, Rleft, Rright, code);
    }
  }
}


void LIR_Assembler::comp_fl2i(LIR_Code code, LIR_Opr left, LIR_Opr right, LIR_Opr dst, LIR_Op2* op) {
  if (code == lir_cmp_fd2i || code == lir_ucmp_fd2i) {
    bool is_unordered_less = (code == lir_ucmp_fd2i);
    if (left->is_single_fpu()) {
      __ float_cmp(true, is_unordered_less ? -1 : 1,
                   left->as_float_reg(), right->as_float_reg(), dst->as_register());
    } else if (left->is_double_fpu()) {
      __ float_cmp(false, is_unordered_less ? -1 : 1,
                   left->as_double_reg(), right->as_double_reg(), dst->as_register());
    } else {
      ShouldNotReachHere();
    }
  } else if (code == lir_cmp_l2i) {
    __ cmp_l2i(dst->as_register(), left->as_register_lo(), right->as_register_lo());
  } else {
    ShouldNotReachHere();
  }
}

void LIR_Assembler::align_call(LIR_Code code) {  }

void LIR_Assembler::call(LIR_OpJavaCall* op, relocInfo::relocType rtype) {
  address call = __ trampoline_call(Address(op->addr(), rtype));
  if (call == NULL) {
    bailout("trampoline stub overflow");
    return;
  }
  add_call_info(code_offset(), op->info());
}

void LIR_Assembler::ic_call(LIR_OpJavaCall* op) {
  address call = __ ic_call(op->addr());
  if (call == NULL) {
    bailout("trampoline stub overflow");
    return;
  }
  add_call_info(code_offset(), op->info());
}

void LIR_Assembler::vtable_call(LIR_OpJavaCall* op) { ShouldNotReachHere(); }

void LIR_Assembler::emit_static_call_stub() {
  address call_pc = __ pc();
  address stub = __ start_a_stub(call_stub_size());
  if (stub == NULL) {
    bailout("static call stub overflow");
    return;
  }

  int start = __ offset();

  __ relocate(static_stub_Relocation::spec(call_pc));
  __ emit_static_call_stub();

  assert(__ offset() - start + CompiledStaticCall::to_trampoline_stub_size() <= call_stub_size(), "stub too big");
  __ end_a_stub();
}

void LIR_Assembler::throw_op(LIR_Opr exceptionPC, LIR_Opr exceptionOop, CodeEmitInfo* info) {
  assert(exceptionOop->as_register() == x10, "must match");
  assert(exceptionPC->as_register() == x13, "must match");

  // exception object is not added to oop map by LinearScan
  // (LinearScan assumes that no oops are in fixed registers)
  info->add_register_oop(exceptionOop);
  Runtime1::StubID unwind_id;

  // get current pc information
  // pc is only needed if the method has an exception handler, the unwind code does not need it.
  if (compilation()->debug_info_recorder()->last_pc_offset() == __ offset()) {
    // As no instructions have been generated yet for this LIR node it's
    // possible that an oop map already exists for the current offset.
    // In that case insert an dummy NOP here to ensure all oop map PCs
    // are unique. See JDK-8237483.
    __ nop();
  }
  int pc_for_athrow_offset = __ offset();
  InternalAddress pc_for_athrow(__ pc());
  int32_t off = 0;
  __ la_patchable(exceptionPC->as_register(), pc_for_athrow, off);
  __ addi(exceptionPC->as_register(), exceptionPC->as_register(), off);
  add_call_info(pc_for_athrow_offset, info); // for exception handler

  __ verify_not_null_oop(x10);
  // search an exception handler (x10: exception oop, x13: throwing pc)
  if (compilation()->has_fpu_code()) {
    unwind_id = Runtime1::handle_exception_id;
  } else {
    unwind_id = Runtime1::handle_exception_nofpu_id;
  }
  __ far_call(RuntimeAddress(Runtime1::entry_for(unwind_id)));
  __ nop();
}

void LIR_Assembler::unwind_op(LIR_Opr exceptionOop) {
  assert(exceptionOop->as_register() == x10, "must match");
  __ j(_unwind_handler_entry);
}


void LIR_Assembler::shift_op(LIR_Code code, LIR_Opr left, LIR_Opr count, LIR_Opr dest, LIR_Opr tmp) {
  Register left_reg = left->is_single_cpu() ? left->as_register() : left->as_register_lo();
  Register dest_reg = dest->is_single_cpu() ? dest->as_register() : dest->as_register_lo();
  Register count_reg = count->as_register();
  if (dest->is_single_cpu()) {
    assert (dest->type() == T_INT, "unexpected result type");
    assert (left->type() == T_INT, "unexpected left type");
    __ andi(t0, count_reg, 31); // should not shift more than 31 bits
    switch (code) {
      case lir_shl:  __ sllw(dest_reg, left_reg, t0); break;
      case lir_shr:  __ sraw(dest_reg, left_reg, t0); break;
      case lir_ushr: __ srlw(dest_reg, left_reg, t0); break;
      default: ShouldNotReachHere();
    }
  } else if (dest->is_double_cpu()) {
    __ andi(t0, count_reg, 63); // should not shift more than 63 bits
    switch (code) {
      case lir_shl:  __ sll(dest_reg, left_reg, t0); break;
      case lir_shr:  __ sra(dest_reg, left_reg, t0); break;
      case lir_ushr: __ srl(dest_reg, left_reg, t0); break;
      default: ShouldNotReachHere();
    }
  } else {
    ShouldNotReachHere();
  }
}


void LIR_Assembler::shift_op(LIR_Code code, LIR_Opr left, jint count, LIR_Opr dest) {
  Register left_reg = left->is_single_cpu() ? left->as_register() : left->as_register_lo();
  Register dest_reg = dest->is_single_cpu() ? dest->as_register() : dest->as_register_lo();
  if (dest->is_single_cpu()) {
    assert (dest->type() == T_INT, "unexpected result type");
    assert (left->type() == T_INT, "unexpected left type");
    count &= 0x1f;
    if (count != 0) {
      switch (code) {
        case lir_shl:  __ slliw(dest_reg, left_reg, count); break;
        case lir_shr:  __ sraiw(dest_reg, left_reg, count); break;
        case lir_ushr: __ srliw(dest_reg, left_reg, count); break;
        default: ShouldNotReachHere();
      }
    } else {
      move_regs(left_reg, dest_reg);
    }
  } else if (dest->is_double_cpu()) {
    count &= 0x3f;
    if (count != 0) {
      switch (code) {
        case lir_shl:  __ slli(dest_reg, left_reg, count); break;
        case lir_shr:  __ srai(dest_reg, left_reg, count); break;
        case lir_ushr: __ srli(dest_reg, left_reg, count); break;
        default: ShouldNotReachHere();
      }
    } else {
      move_regs(left->as_register_lo(), dest->as_register_lo());
    }
  } else {
    ShouldNotReachHere();
  }
}



void LIR_Assembler::emit_lock(LIR_OpLock* op) {
  Register obj = op->obj_opr()->as_register();  // may not be an oop
  Register hdr = op->hdr_opr()->as_register();
  Register lock = op->lock_opr()->as_register();
  if (!UseFastLocking) {
    __ j(*op->stub()->entry());
  } else if (op->code() == lir_lock) {
    Register scratch = noreg;
    if (UseBiasedLocking) {
      scratch = op->scratch_opr()->as_register();
    }
    assert(BasicLock::displaced_header_offset_in_bytes() == 0, "lock_reg must point to the displaced header");
    // add debug info for NullPointerException only if one is possible
    int null_check_offset = __ lock_object(hdr, obj, lock, scratch, *op->stub()->entry());
    if (op->info() != NULL) {
      add_debug_info_for_null_check(null_check_offset, op->info());
    }
  } else if (op->code() == lir_unlock) {
    assert(BasicLock::displaced_header_offset_in_bytes() == 0, "lock_reg must point to the displaced header");
    __ unlock_object(hdr, obj, lock, *op->stub()->entry());
  } else {
    Unimplemented();
  }
  __ bind(*op->stub()->continuation());
}

void LIR_Assembler::emit_profile_call(LIR_OpProfileCall* op) {
  ciMethod* method = op->profiled_method();
  int bci          = op->profiled_bci();

  // Update counter for all call types
  ciMethodData* md = method->method_data_or_null();
  assert(md != NULL, "Sanity");
  ciProfileData* data = md->bci_to_data(bci);
  assert(data != NULL && data->is_CounterData(), "need CounterData for calls");
  assert(op->mdo()->is_single_cpu(),  "mdo must be allocated");
  Register mdo  = op->mdo()->as_register();
  __ mov_metadata(mdo, md->constant_encoding());
  Address counter_addr(mdo, md->byte_offset_of_slot(data, CounterData::count_offset()));
  // Perform additional virtual call profiling for invokevirtual and
  // invokeinterface bytecodes
  if (op->should_profile_receiver_type()) {
    assert(op->recv()->is_single_cpu(), "recv must be allocated");
    Register recv = op->recv()->as_register();
    assert_different_registers(mdo, recv);
    assert(data->is_VirtualCallData(), "need VirtualCallData for virtual calls");
    ciKlass* known_klass = op->known_holder();
    if (C1OptimizeVirtualCallProfiling && known_klass != NULL) {
      // We know the type that will be seen at this call site; we can
      // statically update the MethodData* rather than needing to do
      // dynamic tests on the receiver type
      // NOTE: we should probably put a lock around this search to
      // avoid collisions by concurrent compilations
      ciVirtualCallData* vc_data = (ciVirtualCallData*) data;
      uint i;
      for (i = 0; i < VirtualCallData::row_limit(); i++) {
        ciKlass* receiver = vc_data->receiver(i);
        if (known_klass->equals(receiver)) {
          Address data_addr(mdo, md->byte_offset_of_slot(data, VirtualCallData::receiver_count_offset(i)));
          __ add_memory_int64(data_addr, DataLayout::counter_increment);
          return;
        }
      }

      // Receiver type not found in profile data; select an empty slot
      // Note that this is less efficient than it should be because it
      // always does a write to the receiver part of the
      // VirtualCallData rather than just the first time
      for (i = 0; i < VirtualCallData::row_limit(); i++) {
        ciKlass* receiver = vc_data->receiver(i);
        if (receiver == NULL) {
          Address recv_addr(mdo, md->byte_offset_of_slot(data, VirtualCallData::receiver_offset(i)));
          __ mov_metadata(t1, known_klass->constant_encoding());
          __ sd(t1, recv_addr);
          Address data_addr(mdo, md->byte_offset_of_slot(data, VirtualCallData::receiver_count_offset(i)));
          __ add_memory_int64(data_addr, DataLayout::counter_increment);
          return;
        }
      }
    } else {
      __ load_klass(recv, recv);
      Label update_done;
      type_profile_helper(mdo, md, data, recv, &update_done);
      // Receiver did not match any saved receiver and there is no empty row for it.
      // Increment total counter to indicate polymorphic case.
      __ add_memory_int64(counter_addr, DataLayout::counter_increment);

      __ bind(update_done);
    }
  } else {
    // Static call
    __ add_memory_int64(counter_addr, DataLayout::counter_increment);
  }
}

void LIR_Assembler::emit_delay(LIR_OpDelay*) { Unimplemented(); }

void LIR_Assembler::monitor_address(int monitor_no, LIR_Opr dst) {
  __ la(dst->as_register(), frame_map()->address_for_monitor_lock(monitor_no));
}

void LIR_Assembler::emit_updatecrc32(LIR_OpUpdateCRC32* op) { Unimplemented(); }

void LIR_Assembler::check_conflict(ciKlass* exact_klass, intptr_t current_klass,
                                   Register tmp, Label &next, Label &none,
                                   Address mdo_addr) {
  if (exact_klass == NULL || TypeEntries::is_type_none(current_klass)) {
    if (exact_klass != NULL) {
      __ mov_metadata(tmp, exact_klass->constant_encoding());
    } else {
      __ load_klass(tmp, tmp);
    }

    __ ld(t1, mdo_addr);
    __ xorr(tmp, tmp, t1);
    __ andi(t0, tmp, TypeEntries::type_klass_mask);
    // klass seen before, nothing to do. The unknown bit may have been
    // set already but no need to check.
    __ beqz(t0, next);

    // already unknown. Nothing to do anymore.
    __ andi(t0, tmp, TypeEntries::type_unknown);
    __ bnez(t0, next);

    if (TypeEntries::is_type_none(current_klass)) {
      __ beqz(t1, none);
      __ li(t0, TypeEntries::null_seen);
      __ beq(t0, t1, none);
      // There is a chance that the checks above (re-reading profiling
      // data from memory) fail if another thread has just set the
      // profiling to this obj's klass
      __ membar(MacroAssembler::LoadLoad);
      __ ld(t1, mdo_addr);
      __ xorr(tmp, tmp, t1);
      __ andi(t0, tmp, TypeEntries::type_klass_mask);
      __ beqz(t0, next);
    }
  } else {
    assert(ciTypeEntries::valid_ciklass(current_klass) != NULL &&
           ciTypeEntries::valid_ciklass(current_klass) != exact_klass, "conflict only");

    __ ld(tmp, mdo_addr);
    // already unknown. Nothing to do anymore.
    __ andi(t0, tmp, TypeEntries::type_unknown);
    __ bnez(t0, next);
  }

  // different than before. Cannot keep accurate profile.
  __ ld(t1, mdo_addr);
  __ ori(t1, t1, TypeEntries::type_unknown);
  __ sd(t1, mdo_addr);

  if (TypeEntries::is_type_none(current_klass)) {
    __ j(next);

    __ bind(none);
    // first time here. Set profile type.
    __ sd(tmp, mdo_addr);
  }
}

void LIR_Assembler::check_no_conflict(ciKlass* exact_klass, intptr_t current_klass, Register tmp,
                                      Address mdo_addr, Label &next) {
  // There's a single possible klass at this profile point
  assert(exact_klass != NULL, "should be");
  if (TypeEntries::is_type_none(current_klass)) {
    __ mov_metadata(tmp, exact_klass->constant_encoding());
    __ ld(t1, mdo_addr);
    __ xorr(tmp, tmp, t1);
    __ andi(t0, tmp, TypeEntries::type_klass_mask);
    __ beqz(t0, next);
#ifdef ASSERT
  {
    Label ok;
    __ ld(t0, mdo_addr);
    __ beqz(t0, ok);
    __ li(t1, TypeEntries::null_seen);
    __ beq(t0, t1, ok);
    // may have been set by another thread
    __ membar(MacroAssembler::LoadLoad);
    __ mov_metadata(t0, exact_klass->constant_encoding());
    __ ld(t1, mdo_addr);
    __ xorr(t1, t0, t1);
    __ andi(t1, t1, TypeEntries::type_mask);
    __ beqz(t1, ok);

    __ stop("unexpected profiling mismatch");
    __ bind(ok);
  }
#endif
    // first time here. Set profile type.
    __ ld(tmp, mdo_addr);
  } else {
    assert(ciTypeEntries::valid_ciklass(current_klass) != NULL &&
           ciTypeEntries::valid_ciklass(current_klass) != exact_klass, "inconsistent");

    __ ld(tmp, mdo_addr);
    // already unknown. Nothing to do anymore.
    __ andi(t0, tmp, TypeEntries::type_unknown);
    __ bnez(t0, next);

    __ ori(tmp, tmp, TypeEntries::type_unknown);
    __ sd(tmp, mdo_addr);
  }
}

void LIR_Assembler::check_null(Register tmp, Label &update, intptr_t current_klass,
                               Address mdo_addr, bool do_update, Label &next) {
  __ bnez(tmp, update);
  if (!TypeEntries::was_null_seen(current_klass)) {
    __ ld(t1, mdo_addr);
    __ ori(t1, t1, TypeEntries::null_seen);
    __ sd(t1, mdo_addr);
  }
  if (do_update) {
    __ j(next);
  }
}

void LIR_Assembler::emit_profile_type(LIR_OpProfileType* op) {
  COMMENT("emit_profile_type {");
  Register obj = op->obj()->as_register();
  Register tmp = op->tmp()->as_pointer_register();
  Address mdo_addr = as_Address(op->mdp()->as_address_ptr());
  ciKlass* exact_klass = op->exact_klass();
  intptr_t current_klass = op->current_klass();
  bool not_null = op->not_null();
  bool no_conflict = op->no_conflict();

  Label update, next, none;

  bool do_null = !not_null;
  bool exact_klass_set = exact_klass != NULL && ciTypeEntries::valid_ciklass(current_klass) == exact_klass;
  bool do_update = !TypeEntries::is_type_unknown(current_klass) && !exact_klass_set;

  assert(do_null || do_update, "why are we here?");
  assert(!TypeEntries::was_null_seen(current_klass) || do_update, "why are we here?");
  assert_different_registers(tmp, t0, t1, mdo_addr.base());

  __ verify_oop(obj);

  if (tmp != obj) {
    __ mv(tmp, obj);
  }
  if (do_null) {
    check_null(tmp, update, current_klass, mdo_addr, do_update, next);
#ifdef ASSERT
  } else {
    __ bnez(tmp, update);
    __ stop("unexpected null obj");
#endif
  }

  __ bind(update);

  if (do_update) {
#ifdef ASSERT
    if (exact_klass != NULL) {
      check_exact_klass(tmp, exact_klass);
    }
#endif
    if (!no_conflict) {
      check_conflict(exact_klass, current_klass, tmp, next, none, mdo_addr);
    } else {
      check_no_conflict(exact_klass, current_klass, tmp, mdo_addr, next);
    }

    __ bind(next);
  }
  COMMENT("} emit_profile_type");
}

void LIR_Assembler::align_backward_branch_target() { }

void LIR_Assembler::negate(LIR_Opr left, LIR_Opr dest, LIR_Opr tmp) {
  // tmp must be unused
  assert(tmp->is_illegal(), "wasting a register if tmp is allocated");

  if (left->is_single_cpu()) {
    assert(dest->is_single_cpu(), "expect single result reg");
    __ negw(dest->as_register(), left->as_register());
  } else if (left->is_double_cpu()) {
    assert(dest->is_double_cpu(), "expect double result reg");
    __ neg(dest->as_register_lo(), left->as_register_lo());
  } else if (left->is_single_fpu()) {
    assert(dest->is_single_fpu(), "expect single float result reg");
    __ fneg_s(dest->as_float_reg(), left->as_float_reg());
  } else {
    assert(left->is_double_fpu(), "expect double float operand reg");
    assert(dest->is_double_fpu(), "expect double float result reg");
    __ fneg_d(dest->as_double_reg(), left->as_double_reg());
  }
}


void LIR_Assembler::leal(LIR_Opr addr, LIR_Opr dest, LIR_PatchCode patch_code, CodeEmitInfo* info) {
#if INCLUDE_SHENANDOAHGC
  if (UseShenandoahGC && patch_code != lir_patch_none) {
    deoptimize_trap(info);
    return;
  }
#endif
  assert(patch_code == lir_patch_none, "Patch code not supported");
  LIR_Address* adr = addr->as_address_ptr();
  Register dst = dest->as_register_lo();

  assert_different_registers(dst, t0);
  if(adr->base()->is_valid() && dst == adr->base()->as_pointer_register() && (!adr->index()->is_cpu_register())) {

    intptr_t offset = adr->disp();
    LIR_Opr index_op = adr->index();
    int scale = adr->scale();
    if(index_op->is_constant()) {
      offset += index_op->as_constant_ptr()->as_jint() << scale;
    }

    if(!is_imm_in_range(offset, 12, 0)) {
      __ la(t0, as_Address(adr));
      __ mv(dst, t0);
      return;
    }
  }
  __ la(dst, as_Address(adr));
}


void LIR_Assembler::rt_call(LIR_Opr result, address dest, const LIR_OprList* args, LIR_Opr tmp, CodeEmitInfo* info) {
  assert(!tmp->is_valid(), "don't need temporary");

  CodeBlob *cb = CodeCache::find_blob(dest);
  if (cb != NULL) {
    __ far_call(RuntimeAddress(dest));
  } else {
    int32_t offset = 0;
    __ la_patchable(t0, RuntimeAddress(dest), offset);
    __ jalr(x1, t0, offset);
  }

  if (info != NULL) {
    add_call_info_here(info);
  }
  __ ifence();
}

void LIR_Assembler::volatile_move_op(LIR_Opr src, LIR_Opr dest, BasicType type, CodeEmitInfo* info) {
  if (dest->is_address() || src->is_address()) {
    move_op(src, dest, type, lir_patch_none, info, /* pop_fpu_stack */ false,
           /* unaligned */ false, /* wide */ false);
  } else {
    ShouldNotReachHere();
  }
}

#ifdef ASSERT
// emit run-time assertion
void LIR_Assembler::emit_assert(LIR_OpAssert* op) {
  assert(op->code() == lir_assert, "must be");

  Label ok;
  if (op->in_opr1()->is_valid()) {
    assert(op->in_opr2()->is_valid(), "both operands must be valid");
    bool is_unordered = false;
    LIR_Condition cond = op->condition();
    emit_branch(cond, op->in_opr1(), op->in_opr2(), ok, /* is_far */ false,
                /* is_unordered */(cond == lir_cond_greaterEqual || cond == lir_cond_greater) ? false : true);
  } else {
    assert(op->in_opr2()->is_illegal(), "both operands must be illegal");
    assert(op->condition() == lir_cond_always, "no other conditions allowed");
  }

  if (op->halt()) {
    const char* str = __ code_string(op->msg());
    __ stop(str);
  } else {
    breakpoint();
  }
  __ bind(ok);
}
#endif

#ifndef PRODUCT
#define COMMENT(x)   do { __ block_comment(x); } while (0)
#else
#define COMMENT(x)
#endif

void LIR_Assembler::membar() {
  COMMENT("membar");
  __ membar(MacroAssembler::AnyAny);
}

void LIR_Assembler::membar_acquire() {
  __ membar(MacroAssembler::LoadLoad | MacroAssembler::LoadStore);
}

void LIR_Assembler::membar_release() {
  __ membar(MacroAssembler::LoadStore | MacroAssembler::StoreStore);
}

void LIR_Assembler::membar_loadload() {
  __ membar(MacroAssembler::LoadLoad);
}

void LIR_Assembler::membar_storestore() {
  __ membar(MacroAssembler::StoreStore);
}

void LIR_Assembler::membar_loadstore() { __ membar(MacroAssembler::LoadStore); }

void LIR_Assembler::membar_storeload() { __ membar(MacroAssembler::StoreLoad); }

void LIR_Assembler::on_spin_wait() {
  Unimplemented();
}

void LIR_Assembler::get_thread(LIR_Opr result_reg) {
  __ mv(result_reg->as_register(), xthread);
}

void LIR_Assembler::peephole(LIR_List *lir) {}

void LIR_Assembler::atomic_op(LIR_Code code, LIR_Opr src, LIR_Opr data, LIR_Opr dest, LIR_Opr tmp_op) {
  Address addr = as_Address(src->as_address_ptr());
  BasicType type = src->type();
  bool is_oop = type == T_OBJECT || type == T_ARRAY;

  get_op(type);

  switch (code) {
    case lir_xadd:
      {
        RegisterOrConstant inc;
        Register tmp = as_reg(tmp_op);
        Register dst = as_reg(dest);
        if (data->is_constant()) {
          inc = RegisterOrConstant(as_long(data));
          assert_different_registers(dst, addr.base(), tmp);
          assert_different_registers(tmp, t0);
        } else {
          inc = RegisterOrConstant(as_reg(data));
          assert_different_registers(inc.as_register(), dst, addr.base(), tmp);
        }
        __ la(tmp, addr);
        (_masm->*add)(dst, inc, tmp);
        break;
      }
    case lir_xchg:
      {
        Register tmp = tmp_op->as_register();
        Register obj = as_reg(data);
        Register dst = as_reg(dest);
        if (is_oop && UseCompressedOops) {
          __ encode_heap_oop(t0, obj);
          obj = t0;
        }
        assert_different_registers(obj, addr.base(), tmp, dst);
        __ la(tmp, addr);
        (_masm->*xchg)(dst, obj, tmp);
        if (is_oop && UseCompressedOops) {
          __ decode_heap_oop(dst);
        }
      }
      break;
    default:
      ShouldNotReachHere();
  }
  __ membar(MacroAssembler::AnyAny);
}

int LIR_Assembler::array_element_size(BasicType type) const {
  int elem_size = type2aelembytes(type);
  return exact_log2(elem_size);
}

Address LIR_Assembler::as_Address(LIR_Address* addr, Register tmp) {
  if (addr->base()->is_illegal()) {
    assert(addr->index()->is_illegal(), "must be illegal too");
    __ movptr(tmp, addr->disp());
    return Address(tmp, 0);
  }

  Register base = addr->base()->as_pointer_register();
  LIR_Opr index_op = addr->index();
  int scale = addr->scale();

  if (index_op->is_illegal()) {
    return Address(base, addr->disp());
  } else if (index_op->is_cpu_register()) {
    Register index;
    if (index_op->is_single_cpu()) {
      index = index_op->as_register();
    } else {
      index = index_op->as_register_lo();
    }
    if (scale != 0) {
      __ slli(tmp, index, scale);
      __ add(tmp, base, tmp);
    } else {
      __ add(tmp, base, index);
    }
    return Address(tmp, addr->disp());
  } else if (index_op->is_constant()) {
    intptr_t addr_offset = (index_op->as_constant_ptr()->as_jint() << scale) + addr->disp();
    return Address(base, addr_offset);
  }

  Unimplemented();
  return Address();
}

// helper functions which checks for overflow and sets bailout if it
// occurs.  Always returns a valid embeddable pointer but in the
// bailout case the pointer won't be to unique storage.
address LIR_Assembler::float_constant(float f) {
  address const_addr = __ float_constant(f);
  if (const_addr == NULL) {
    bailout("const section overflow");
    return __ code()->consts()->start();
  } else {
    return const_addr;
  }
}

address LIR_Assembler::double_constant(double d) {
  address const_addr = __ double_constant(d);
  if (const_addr == NULL) {
    bailout("const section overflow");
    return __ code()->consts()->start();
  } else {
    return const_addr;
  }
}

address LIR_Assembler::int_constant(jlong n) {
  address const_addr = __ long_constant(n);
  if (const_addr == NULL) {
    bailout("const section overflow");
    return __ code()->consts()->start();
  } else {
    return const_addr;
  }
}

void LIR_Assembler::add_debug_info_for_branch(address adr, CodeEmitInfo* info) {
  _masm->code_section()->relocate(adr, relocInfo::poll_type);
  int pc_offset = code_offset();
  flush_debug_info(pc_offset);
  info->record_debug_info(compilation()->debug_info_recorder(), pc_offset);
  if (info->exception_handlers() != NULL) {
    compilation()->add_exception_handlers_for_pco(pc_offset, info->exception_handlers());
  }
}

void LIR_Assembler::casw(Register addr, Register newval, Register cmpval) {
  __ cmpxchg(addr, cmpval, newval, Assembler::int32, Assembler::aq /* acquire */,
             Assembler::rl /* release */, t0, true /* result as bool */);
  __ seqz(t0, t0); // cmpxchg not equal, set t0 to 1
  __ membar(MacroAssembler::AnyAny);
}

void LIR_Assembler::caswu(Register addr, Register newval, Register cmpval) {
  __ cmpxchg(addr, cmpval, newval, Assembler::uint32, Assembler::aq /* acquire */,
             Assembler::rl /* release */, t0, true /* result as bool */);
  __ seqz(t0, t0); // cmpxchg not equal, set t0 to 1
  __ membar(MacroAssembler::AnyAny);
}

void LIR_Assembler::casl(Register addr, Register newval, Register cmpval) {
  __ cmpxchg(addr, cmpval, newval, Assembler::int64, Assembler::aq /* acquire */,
             Assembler::rl /* release */, t0, true /* result as bool */);
  __ seqz(t0, t0); // cmpxchg not equal, set t0 to 1
  __ membar(MacroAssembler::AnyAny);
}

void LIR_Assembler::deoptimize_trap(CodeEmitInfo *info) {
  address target = NULL;

  switch (patching_id(info)) {
    case PatchingStub::access_field_id:
      target = Runtime1::entry_for(Runtime1::access_field_patching_id);
      break;
    case PatchingStub::load_klass_id:
      target = Runtime1::entry_for(Runtime1::load_klass_patching_id);
      break;
    case PatchingStub::load_mirror_id:
      target = Runtime1::entry_for(Runtime1::load_mirror_patching_id);
      break;
    case PatchingStub::load_appendix_id:
      target = Runtime1::entry_for(Runtime1::load_appendix_patching_id);
      break;
    default: ShouldNotReachHere();
  }

  __ far_call(RuntimeAddress(target));
  add_call_info_here(info);
}


void LIR_Assembler::check_exact_klass(Register tmp, ciKlass* exact_klass) {
  Label ok;
  __ load_klass(tmp, tmp);
  __ mov_metadata(t0, exact_klass->constant_encoding());
  __ xorr(t0, tmp, t0);
  __ beqz(t0, ok);
  __ stop("exact klass and actual klass differ");
  __ bind(ok);
}

void LIR_Assembler::get_op(BasicType type) {
  switch (type) {
    case T_INT:
      xchg = &MacroAssembler::atomic_xchgalw;
      add = &MacroAssembler::atomic_addalw;
      break;
    case T_LONG:
      xchg = &MacroAssembler::atomic_xchgal;
      add = &MacroAssembler::atomic_addal;
      break;
    case T_OBJECT:
    case T_ARRAY:
      if (UseCompressedOops) {
        xchg = &MacroAssembler::atomic_xchgalwu;
        add = &MacroAssembler::atomic_addalw;
      } else {
        xchg = &MacroAssembler::atomic_xchgal;
        add = &MacroAssembler::atomic_addal;
      }
      break;
    default:
      ShouldNotReachHere();
  }
}

// emit_opTypeCheck sub functions
void LIR_Assembler::typecheck_lir_store(LIR_OpTypeCheck* op, bool should_profile) {
  Register value = op->object()->as_register();
  Register array = op->array()->as_register();
  Register k_RInfo = op->tmp1()->as_register();
  Register klass_RInfo = op->tmp2()->as_register();
  Register Rtmp1 = op->tmp3()->as_register();

  CodeStub* stub = op->stub();

  // check if it needs to be profiled
  ciMethodData* md = NULL;
  ciProfileData* data = NULL;

  if (should_profile) {
    data_check(op, &md, &data);
  }
  Label profile_cast_success, profile_cast_failure, done;
  Label *success_target = should_profile ? &profile_cast_success : &done;
  Label *failure_target = should_profile ? &profile_cast_failure : stub->entry();

  if (should_profile) {
    profile_object(md, data, value, klass_RInfo, &done);
  } else {
    __ beqz(value, done);
  }

  add_debug_info_for_null_check_here(op->info_for_exception());
  __ load_klass(k_RInfo, array);
  __ load_klass(klass_RInfo, value);

  lir_store_slowcheck(k_RInfo, klass_RInfo, Rtmp1, success_target, failure_target);

  // fall through to the success case
  if (should_profile) {
    Register mdo = klass_RInfo;
    Register recv = k_RInfo;
    __ bind(profile_cast_success);
    __ mov_metadata(mdo, md->constant_encoding());
    __ load_klass(recv, value);
    type_profile_helper(mdo, md, data, recv, &done);
    __ j(done);

    __ bind(profile_cast_failure);
    __ mov_metadata(mdo, md->constant_encoding());
    Address counter_addr(mdo, md->byte_offset_of_slot(data, CounterData::count_offset()));
    __ ld(t1, counter_addr);
    __ addi(t1, t1, -DataLayout::counter_increment);
    __ sd(t1, counter_addr);
    __ j(*stub->entry());
  }

  __ bind(done);
}

void LIR_Assembler::type_profile(Register obj, ciMethodData* md, Register klass_RInfo, Register k_RInfo,
                                 ciProfileData* data, Label* success, Label* failure,
                                 Label& profile_cast_success, Label& profile_cast_failure) {
  Register mdo = klass_RInfo;
  Register recv = k_RInfo;
  __ bind(profile_cast_success);
  __ mov_metadata(mdo, md->constant_encoding());
  __ load_klass(recv, obj);
  Label update_done;
  type_profile_helper(mdo, md, data, recv, success);
  __ j(*success);

  __ bind(profile_cast_failure);
  __ mov_metadata(mdo, md->constant_encoding());
  Address counter_addr = __ form_address(t1, mdo, md->byte_offset_of_slot(data, CounterData::count_offset()));
  __ ld(t0, counter_addr);
  __ addi(t0, t0, -DataLayout::counter_increment);
  __ sd(t0, counter_addr);
  __ j(*failure);
}

void LIR_Assembler::lir_store_slowcheck(Register k_RInfo, Register klass_RInfo, Register Rtmp1,
                                        Label* success_target, Label* failure_target) {
  // get instance klass (it's already uncompressed)
  __ ld(k_RInfo, Address(k_RInfo, ObjArrayKlass::element_klass_offset()));
  // perform the fast part of the checking logic
  __ check_klass_subtype_fast_path(klass_RInfo, k_RInfo, Rtmp1, success_target, failure_target, NULL);
  // call out-of-line instance of __ check_klass_subtype_slow_path(...)
  __ addi(sp, sp, -2 * wordSize); // 2: store k_RInfo and klass_RInfo
  __ sd(klass_RInfo, Address(sp, wordSize));  // sub klass
  __ sd(k_RInfo, Address(sp, 0));             // super klass
  __ far_call(RuntimeAddress(Runtime1::entry_for(Runtime1::slow_subtype_check_id)));
  // load result to k_RInfo
  __ ld(k_RInfo, Address(sp, 0));
  __ addi(sp, sp, 2 * wordSize); // 2: pop out k_RInfo and klass_RInfo
  // result is a boolean
  __ beqz(k_RInfo, *failure_target, /* is_far */ true);
}

void LIR_Assembler::const2reg_helper(LIR_Opr src) {
  switch (src->as_constant_ptr()->type()) {
    case T_INT:
    case T_ADDRESS:
    case T_OBJECT:
    case T_ARRAY:
    case T_METADATA:
        const2reg(src, FrameMap::t0_opr, lir_patch_none, NULL);
        break;
    case T_LONG:
        const2reg(src, FrameMap::t0_long_opr, lir_patch_none, NULL);
        break;
    case T_FLOAT:
    case T_DOUBLE:
    default:
      ShouldNotReachHere();
  }
}

void LIR_Assembler::logic_op_reg32(Register dst, Register left, Register right, LIR_Code code) {
  switch (code) {
    case lir_logic_and: __ andrw(dst, left, right); break;
    case lir_logic_or:  __ orrw (dst, left, right); break;
    case lir_logic_xor: __ xorrw(dst, left, right); break;
    default:            ShouldNotReachHere();
  }
}

void LIR_Assembler::logic_op_reg(Register dst, Register left, Register right, LIR_Code code) {
  switch (code) {
    case lir_logic_and: __ andr(dst, left, right); break;
    case lir_logic_or:  __ orr (dst, left, right); break;
    case lir_logic_xor: __ xorr(dst, left, right); break;
    default:            ShouldNotReachHere();
  }
}

void LIR_Assembler::logic_op_imm(Register dst, Register left, int right, LIR_Code code) {
  switch (code) {
    case lir_logic_and: __ andi(dst, left, right); break;
    case lir_logic_or:  __ ori (dst, left, right); break;
    case lir_logic_xor: __ xori(dst, left, right); break;
    default:            ShouldNotReachHere();
  }
}

void LIR_Assembler::store_parameter(Register r, int offset_from_rsp_in_words) {
  assert(offset_from_rsp_in_words >= 0, "invalid offset from rsp");
  int offset_from_rsp_in_bytes = offset_from_rsp_in_words * BytesPerWord;
  assert(offset_from_rsp_in_bytes < frame_map()->reserved_argument_area_size(), "invalid offset");
  __ sd(r, Address(sp, offset_from_rsp_in_bytes));
}

void LIR_Assembler::store_parameter(jint c, int offset_from_rsp_in_words) {
  assert(offset_from_rsp_in_words >= 0, "invalid offset from rsp");
  int offset_from_rsp_in_bytes = offset_from_rsp_in_words * BytesPerWord;
  assert(offset_from_rsp_in_bytes < frame_map()->reserved_argument_area_size(), "invalid offset");
  __ li(t0, c);
  __ sd(t0, Address(sp, offset_from_rsp_in_bytes));
}

#undef __
