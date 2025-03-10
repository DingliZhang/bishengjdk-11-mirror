/*
 * Copyright (c) 2018, Red Hat, Inc. All rights reserved.
 * Copyright (c) 2020, Huawei Technologies Co., Ltd. All rights reserved.
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

#ifndef CPU_RISCV64_GC_SHENANDOAH_SHENANDOAHBARRIERSETASSEMBLER_RISCV64_HPP
#define CPU_RISCV64_GC_SHENANDOAH_SHENANDOAHBARRIERSETASSEMBLER_RISCV64_HPP

#include "asm/macroAssembler.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#ifdef COMPILER1
class LIR_Assembler;
class ShenandoahPreBarrierStub;
class ShenandoahLoadReferenceBarrierStub;
class StubAssembler;
#endif
class StubCodeGenerator;

class ShenandoahBarrierSetAssembler: public BarrierSetAssembler {
public:
  static address shenandoah_lrb();

  void storeval_barrier(MacroAssembler* masm, Register dst, Register tmp);

#ifdef COMPILER1
  void gen_pre_barrier_stub(LIR_Assembler* ce, ShenandoahPreBarrierStub* stub);
  void gen_load_reference_barrier_stub(LIR_Assembler* ce, ShenandoahLoadReferenceBarrierStub* stub);
  void generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm);
  void generate_c1_load_reference_barrier_runtime_stub(StubAssembler* sasm);
#endif

  virtual void arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, bool is_oop,
                                  Register src, Register dst, Register count, RegSet saved_regs);
  virtual void load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                       Register dst, Address src, Register tmp1, Register tmp_thread);
  virtual void store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                        Address dst, Register val, Register tmp1, Register tmp2);
  virtual void try_resolve_jobject_in_native(MacroAssembler* masm, Register jni_env,
                                             Register obj, Register tmp, Label& slowpath);
  virtual void cmpxchg_oop(MacroAssembler* masm, Register addr, Register expected, Register new_val,
                           Assembler::Aqrl acquire, Assembler::Aqrl release, bool is_cae, Register result);

  virtual void barrier_stubs_init();

private:

  static address _shenandoah_lrb;

  void satb_write_barrier_pre(MacroAssembler* masm,
                              Register obj,
                              Register pre_val,
                              Register thread,
                              Register tmp,
                              bool tosca_live,
                              bool expand_call);
  void shenandoah_write_barrier_pre(MacroAssembler* masm,
                                    Register obj,
                                    Register pre_val,
                                    Register thread,
                                    Register tmp,
                                    bool tosca_live,
                                    bool expand_call);

  void resolve_forward_pointer(MacroAssembler* masm, Register dst, Register tmp = noreg);
  void resolve_forward_pointer_not_null(MacroAssembler* masm, Register dst, Register tmp = noreg);
  void load_reference_barrier(MacroAssembler* masm, Register dst, Address load_addr);
  void load_reference_barrier_not_null(MacroAssembler* masm, Register dst, Address load_addr);

  address generate_shenandoah_lrb(StubCodeGenerator* cgen);
};

#endif // CPU_RISCV64_GC_SHENANDOAH_SHENANDOAHBARRIERSETASSEMBLER_RISCV64_HPP
