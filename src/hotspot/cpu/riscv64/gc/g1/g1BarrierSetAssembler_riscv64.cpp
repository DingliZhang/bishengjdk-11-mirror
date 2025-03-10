/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/macroAssembler.inline.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "gc/g1/g1CardTable.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif

#define __ masm->

void G1BarrierSetAssembler::gen_write_ref_array_pre_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                            Register addr, Register count, RegSet saved_regs) {
  assert_cond(masm != NULL);
  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;
  if (!dest_uninitialized) {
    __ push_reg(saved_regs, sp);
    if (count == c_rarg0) {
      if (addr == c_rarg1) {
        // exactly backwards!!
        __ mv(t0, c_rarg0);
        __ mv(c_rarg0, c_rarg1);
        __ mv(c_rarg1, t0);
      } else {
        __ mv(c_rarg1, count);
        __ mv(c_rarg0, addr);
      }
    } else {
      __ mv(c_rarg0, addr);
      __ mv(c_rarg1, count);
    }
    if (UseCompressedOops) {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_narrow_oop_entry), 2);
    } else {
      __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_pre_oop_entry), 2);
    }
    __ pop_reg(saved_regs, sp);
  }

}

void G1BarrierSetAssembler::gen_write_ref_array_post_barrier(MacroAssembler* masm, DecoratorSet decorators,
                                                             Register start, Register count, Register tmp, RegSet saved_regs) {
  assert_cond(masm != NULL);
  __ push_reg(saved_regs, sp);
  assert_different_registers(start, count, tmp);
  assert_different_registers(c_rarg0, count);
  __ mv(c_rarg0, start);
  __ mv(c_rarg1, count);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_array_post_entry), 2);
  __ pop_reg(saved_regs, sp);
}

void G1BarrierSetAssembler::g1_write_barrier_pre(MacroAssembler* masm,
                                                 Register obj,
                                                 Register pre_val,
                                                 Register thread,
                                                 Register tmp,
                                                 bool tosca_live,
                                                 bool expand_call) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

  assert_cond(masm != NULL);
  assert(thread == xthread, "must be");

  Label done;
  Label runtime;

  assert_different_registers(obj, pre_val, tmp, t0);
  assert(pre_val != noreg &&  tmp != noreg, "expecting a register");

  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  Address index(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset()));

  // Is marking active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) { // 4-byte width
    __ lwu(tmp, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ lbu(tmp, in_progress);
  }
  __ beqz(tmp, done);

  // Do we need to load the previous value?
  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, noreg, AS_RAW);
  }

  // Is the previous value null?
  __ beqz(pre_val, done);

  // Can we store original value in the thread's buffer?
  // Is index == 0?
  // (The index field is typed as size_t.)

  __ ld(tmp, index);                       // tmp := *index_adr
  __ beqz(tmp, runtime);                   // tmp == 0?
                                           // If yes, goto runtime

  __ sub(tmp, tmp, wordSize);              // tmp := tmp - wordSize
  __ sd(tmp, index);                       // *index_adr := tmp
  __ ld(t0, buffer);
  __ add(tmp, tmp, t0);                    // tmp := tmp + *buffer_adr

  // Record the previous value
  __ sd(pre_val, Address(tmp, 0));
  __ j(done);

  __ bind(runtime);
  // save the live input values
  RegSet saved = RegSet::of(pre_val);
  if (tosca_live) { saved += RegSet::of(x10); }
  if (obj != noreg) { saved += RegSet::of(obj); }

  __ push_reg(saved, sp);

  if (expand_call) {
    assert(pre_val != c_rarg1, "smashed arg");
    __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  }

  __ pop_reg(saved, sp);

  __ bind(done);

}

void G1BarrierSetAssembler::g1_write_barrier_post(MacroAssembler* masm,
                                                  Register store_addr,
                                                  Register new_val,
                                                  Register thread,
                                                  Register tmp,
                                                  Register tmp2) {
  assert_cond(masm != NULL);
  assert(thread == xthread, "must be");
  assert_different_registers(store_addr, new_val, thread, tmp, tmp2,
                             t0);
  assert(store_addr != noreg && new_val != noreg && tmp != noreg &&
         tmp2 != noreg, "expecting a register");

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  BarrierSet* bs = BarrierSet::barrier_set();
  CardTableBarrierSet* ctbs = barrier_set_cast<CardTableBarrierSet>(bs);
  CardTable* ct = ctbs->card_table();
  assert(sizeof(*ct->byte_map_base()) == sizeof(jbyte), "adjust this code");

  Label done;
  Label runtime;

  // Does store cross heap regions?

  __ xorr(tmp, store_addr, new_val);
  __ srli(tmp, tmp, HeapRegion::LogOfHRGrainBytes);
  __ beqz(tmp, done);

  // crosses regions, storing NULL?

  __ beqz(new_val, done);

  // storing region crossing non-NULL, is card already dirty?

  ExternalAddress cardtable((address) ct->byte_map_base());
  assert(sizeof(*ct->byte_map_base()) == sizeof(jbyte), "adjust this code");
  const Register card_addr = tmp;

  __ srli(card_addr, store_addr, CardTable::card_shift);

  // get the address of the card
  __ load_byte_map_base(tmp2);
  __ add(card_addr, card_addr, tmp2);
  __ lbu(tmp2, Address(card_addr));
  __ mv(t0, (int)G1CardTable::g1_young_card_val());
  __ beq(tmp2, t0, done);

  assert((int)CardTable::dirty_card_val() == 0, "must be 0");

  __ membar(MacroAssembler::StoreLoad);

  __ lbu(tmp2, Address(card_addr));
  __ beqz(tmp2, done);

  // storing a region crossing, non-NULL oop, card is clean.
  // dirty card and log.

  __ sb(zr, Address(card_addr));

  __ ld(t0, queue_index);
  __ beqz(t0, runtime);
  __ sub(t0, t0, wordSize);
  __ sd(t0, queue_index);

  __ ld(tmp2, buffer);
  __ add(t0, tmp2, t0);
  __ sd(card_addr, Address(t0, 0));
  __ j(done);

  __ bind(runtime);
  // save the live input values
  RegSet saved = RegSet::of(store_addr, new_val);
  __ push_reg(saved, sp);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);
  __ pop_reg(saved, sp);

  __ bind(done);
}

void G1BarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                    Register dst, Address src, Register tmp1, Register tmp_thread) {
  assert_cond(masm != NULL);
  bool on_oop = type == T_OBJECT || type == T_ARRAY;
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool on_reference = on_weak || on_phantom;
  ModRefBarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);
  if (on_oop && on_reference) {
    // LR is live.  It must be saved around calls.
    __ enter(); // barrier may call runtime
    // Generate the G1 pre-barrier code to log the value of
    // the referent field in an SATB buffer.
    g1_write_barrier_pre(masm /* masm */,
                         noreg /* obj */,
                         dst /* pre_val */,
                         xthread /* thread */,
                         tmp1 /* tmp */,
                         true /* tosca_live */,
                         true /* expand_call */);
    __ leave();
  }
}

void G1BarrierSetAssembler::oop_store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                         Address dst, Register val, Register tmp1, Register tmp2) {
  assert_cond(masm != NULL);
  // flatten object address if needed
  if (dst.offset() == 0) {
    __ mv(x13, dst.base());
  } else {
    __ la(x13, dst);
  }

  g1_write_barrier_pre(masm,
                       x13 /* obj */,
                       tmp2 /* pre_val */,
                       xthread /* thread */,
                       tmp1  /* tmp */,
                       val != noreg /* tosca_live */,
                       false /* expand_call */);

  if (val == noreg) {
    BarrierSetAssembler::store_at(masm, decorators, type, Address(x13, 0), noreg, noreg, noreg);
  } else {
    // G1 barrier needs uncompressed oop for region cross check.
    Register new_val = val;
    if (UseCompressedOops) {
      new_val = t1;
      __ mv(new_val, val);
    }
    BarrierSetAssembler::store_at(masm, decorators, type, Address(x13, 0), val, noreg, noreg);
    g1_write_barrier_post(masm,
                          x13 /* store_adr */,
                          new_val /* new_val */,
                          xthread /* thread */,
                          tmp1 /* tmp */,
                          tmp2 /* tmp2 */);
  }
}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void G1BarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, G1PreBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();

  // At this point we know that marking is in progress.
  // If do_load() is true then we have to emit the
  // load of the previous value; otherwise it has already
  // been loaded into _pre_val.
  __ bind(*stub->entry());

  assert(stub->pre_val()->is_register(), "Precondition.");

  Register pre_val_reg = stub->pre_val()->as_register();

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(), stub->info(),
                false /* wide */, false /* unaligned */);
  }
  __ beqz(pre_val_reg, *stub->continuation(), /* is_far */ true);
  ce->store_parameter(stub->pre_val()->as_register(), 0);
  __ far_call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  __ j(*stub->continuation());
}

void G1BarrierSetAssembler::gen_post_barrier_stub(LIR_Assembler* ce, G1PostBarrierStub* stub) {
  G1BarrierSetC1* bs = (G1BarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  __ bind(*stub->entry());
  assert(stub->addr()->is_register(), "Precondition");
  assert(stub->new_val()->is_register(), "Precondition");
  Register new_val_reg = stub->new_val()->as_register();
  __ beqz(new_val_reg, *stub->continuation(), /* is_far */ true);
  ce->store_parameter(stub->addr()->as_pointer_register(), 0);
  __ far_call(RuntimeAddress(bs->post_barrier_c1_runtime_code_blob()->code_begin()));
  __ j(*stub->continuation());
}

#undef __

#define __ sasm->

void G1BarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_pre_barrier", false);

  BarrierSet* bs = BarrierSet::barrier_set();

  // arg0 : previous value of memory
  const Register pre_val = x10;
  const Register thread = xthread;
  const Register tmp = t0;

  Address in_progress(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_active_offset()));
  Address queue_index(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  // Is marking still active?
  if (in_bytes(SATBMarkQueue::byte_width_of_active()) == 4) {  // 4-byte width
    __ lwu(tmp, in_progress);
  } else {
    assert(in_bytes(SATBMarkQueue::byte_width_of_active()) == 1, "Assumption");
    __ lbu(tmp, in_progress);
  }
  __ beqz(tmp, done);

  // Can we store original value in the thread's buffer?
  __ ld(tmp, queue_index);
  __ beqz(tmp, runtime);

  __ sub(tmp, tmp, wordSize);
  __ sd(tmp, queue_index);
  __ ld(t1, buffer);
  __ add(tmp, tmp, t1);
  __ load_parameter(0, t1);
  __ sd(t1, Address(tmp, 0));
  __ j(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ load_parameter(0, pre_val);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_pre_entry), pre_val, thread);
  __ pop_call_clobbered_registers();
  __ bind(done);

  __ epilogue();
}

void G1BarrierSetAssembler::generate_c1_post_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("g1_post_barrier", false);

  // arg0 : store_address
  Address store_addr(fp, 2 * BytesPerWord); // 2 BytesPerWord from fp

  BarrierSet* bs = BarrierSet::barrier_set();
  CardTableBarrierSet* ctbs = barrier_set_cast<CardTableBarrierSet>(bs);
  CardTable* ct = ctbs->card_table();
  assert(sizeof(*ct->byte_map_base()) == sizeof(jbyte), "adjust this code");

  Label done;
  Label runtime;

  // At this point we know new_value is non-NULL and the new_value crosses regions.
  // Must check to see if card is already dirty
  const Register thread = xthread;

  Address queue_index(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_index_offset()));
  Address buffer(thread, in_bytes(G1ThreadLocalData::dirty_card_queue_buffer_offset()));

  const Register card_offset = t1;
  // LR is free here, so we can use it to hold the byte_map_base.
  const Register byte_map_base = lr;

  assert_different_registers(card_offset, byte_map_base, t0);

  __ load_parameter(0, card_offset);
  __ srli(card_offset, card_offset, CardTable::card_shift);
  __ load_byte_map_base(byte_map_base);

  // Convert card offset into an address in card_addr
  Register card_addr = card_offset;
  __ add(card_addr, byte_map_base, card_addr);

  __ lbu(t0, Address(card_addr, 0));
  __ sub(t0, t0, (int)G1CardTable::g1_young_card_val());
  __ beqz(t0, done);

  assert((int)CardTable::dirty_card_val() == 0, "must be 0");

  __ membar(MacroAssembler::StoreLoad);
  __ lbu(t0, Address(card_addr, 0));
  __ beqz(t0, done);

  // storing region crossing non-NULL, card is clean.
  // dirty card and log.
  __ sb(zr, Address(card_addr, 0));

  __ ld(t0, queue_index);
  __ beqz(t0, runtime);
  __ sub(t0, t0, wordSize);
  __ sd(t0, queue_index);

  // Reuse LR to hold buffer_addr
  const Register buffer_addr = lr;

  __ ld(buffer_addr, buffer);
  __ add(t0, buffer_addr, t0);
  __ sd(card_addr, Address(t0, 0));
  __ j(done);

  __ bind(runtime);
  __ push_call_clobbered_registers();
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, G1BarrierSetRuntime::write_ref_field_post_entry), card_addr, thread);
  __ pop_call_clobbered_registers();
  __ bind(done);
  __ epilogue();
}

#undef __

#endif // COMPILER1
