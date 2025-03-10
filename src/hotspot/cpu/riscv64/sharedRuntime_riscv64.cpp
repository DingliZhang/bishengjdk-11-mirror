/*
 * Copyright (c) 2003, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2019, Red Hat Inc. All rights reserved.
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
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/debugInfoRec.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interp_masm.hpp"
#include "interpreter/interpreter.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/compiledICHolder.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/vframeArray.hpp"
#include "utilities/align.hpp"
#include "vmreg_riscv64.inline.hpp"
#ifdef COMPILER1
#include "c1/c1_Runtime1.hpp"
#endif
#ifdef COMPILER2
#include "adfiles/ad_riscv64.hpp"
#include "opto/runtime.hpp"
#endif

#define __ masm->

const int StackAlignmentInSlots = StackAlignmentInBytes / VMRegImpl::stack_slot_size;

class SimpleRuntimeFrame {

 public:

  // Most of the runtime stubs have this simple frame layout.
  // This class exists to make the layout shared in one place.
  // Offsets are for compiler stack slots, which are jints.
  enum layout {
    // The frame sender code expects that fp will be in the "natural" place and
    // will override any oopMap setting for it. We must therefore force the layout
    // so that it agrees with the frame sender code.
    // we don't expect any arg reg save area so riscv64 asserts that
    // frame::arg_reg_save_area_bytes == 0
    fp_off = 0, fp_off2,
    return_off, return_off2,
    framesize
  };
};

class RegisterSaver {
 public:
  static OopMap* save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words);
  static void restore_live_registers(MacroAssembler* masm);

  // Offsets into the register save area
  // Used by deoptimization when it is managing result register
  // values on its own
  // gregs:30, float_register:32; except: x1 & x2
  // |---f0---|<---SP
  // |---f1---|
  // |   ..   |
  // |---f31--|
  // |---zr---|
  // |---x3---|
  // |   x4   |
  // |---.. --|
  // |---x31--|
  // |---fp---|
  // |---ra---|
  static int reg_offset_in_bytes(Register r) {
    assert (r->encoding() > 2, "ra and sp not saved");
    return (32 /* floats*/ + r->encoding() - 2 /* x1, x2*/) * wordSize;
  }
  static int x10_offset_in_bytes(void)        { return reg_offset_in_bytes(x10); } // x10
  static int xmethod_offset_in_bytes(void)    { return reg_offset_in_bytes(xmethod); } // x31
  static int tmp0_offset_in_bytes(void)       { return reg_offset_in_bytes(t0); } // x5
  static int f0_offset_in_bytes(void)         { return 0; }
  static int f10_offset_in_bytes(void)        { return 10 /* floats*/ * wordSize; }
  static int return_offset_in_bytes(void)     { return return_off * BytesPerInt; }

  // During deoptimization only the result registers need to be restored,
  // all the other values have already been extracted.
  static void restore_result_registers(MacroAssembler* masm);

  // Capture info about frame layout
  enum layout {
    fpu_state_off = 0,
    fpu_state_end = fpu_state_off + FPUStateSizeInWords - 1,
    // The frame sender code expects that fp will be in
    // the "natural" place and will override any oopMap
    // setting for it. We must therefore force the layout
    // so that it agrees with the frame sender code.
    x0_off        = fpu_state_off + FPUStateSizeInWords,
    fp_off        = x0_off + 30 * 2,
    return_off    = fp_off + 2,      // slot for return address
    reg_save_size = return_off + 2
  };
};

OopMap* RegisterSaver::save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words) {
  assert_cond(masm != NULL && total_frame_words != NULL);
  int frame_size_in_bytes = align_up(additional_frame_words * wordSize + reg_save_size * BytesPerInt, 16);
  // OopMap frame size is in compiler stack slots (jint's) not bytes or words
  int frame_size_in_slots = frame_size_in_bytes / BytesPerInt;
  // The caller will allocate additional_frame_words
  int additional_frame_slots = additional_frame_words * wordSize / BytesPerInt;
  // CodeBlob frame size is in words.
  int frame_size_in_words = frame_size_in_bytes / wordSize;
  *total_frame_words = frame_size_in_words;

  // Save Integer and Float registers.
  __ enter();
  __ push_CPU_state();

  // Set an oopmap for the call site.  This oopmap will map all
  // oop-registers and debug-info registers as callee-saved.  This
  // will allow deoptimization at this safepoint to find all possible
  // debug-info recordings, as well as let GC find all oops.

  OopMapSet *oop_maps = new OopMapSet();
  OopMap* oop_map = new OopMap(frame_size_in_slots, 0);
  assert_cond(oop_maps != NULL && oop_map != NULL);

  // ignore zr, ra and sp, being ignored also by push_CPU_state (pushing zr only for stack alignment)
  for (int i = 3; i < RegisterImpl::number_of_registers; i++) {
    Register r = as_Register(i);
    if (r != xthread && r != t0 && r != t1) {
      int sp_offset = 2 * ((i - 2) + 32); // SP offsets are in 4-byte words, register slots are 8 bytes
                                          // wide, 32 floating-point registers
      oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset + additional_frame_slots), r->as_VMReg());
    }
  }

  for (int i = 0; i < FloatRegisterImpl::number_of_registers; i++) {
    FloatRegister r = as_FloatRegister(i);
    int sp_offset = 2 * i;
    oop_map->set_callee_saved(VMRegImpl::stack2reg(sp_offset), r->as_VMReg());
  }

  return oop_map;
}

void RegisterSaver::restore_live_registers(MacroAssembler* masm) {
  assert_cond(masm != NULL);
  __ pop_CPU_state();
  __ leave();
}

void RegisterSaver::restore_result_registers(MacroAssembler* masm) {
  // Just restore result register. Only used by deoptimization. By
  // now any callee save register that needs to be restored to a c2
  // caller of the deoptee has been extracted into the vframeArray
  // and will be stuffed into the c2i adapter we create for later
  // restoration so only result registers need to be restored here.
  assert_cond(masm != NULL);
  // Restore fp result register
  __ fld(f10, Address(sp, f10_offset_in_bytes()));
  // Restore integer result register
  __ ld(x10, Address(sp, x10_offset_in_bytes()));

  // Pop all of the register save are off the stack
  __ add(sp, sp, align_up(return_offset_in_bytes(), 16));
}

// Is vector's size (in bytes) bigger than a size saved by default?
// 8 bytes vector registers are saved by default on riscv64.
bool SharedRuntime::is_wide_vector(int size) {
  return size > 8;
}

size_t SharedRuntime::trampoline_size() {
  // Byte size of function generate_trampoline. movptr_with_offset: 5 instructions, jalr: 1 instrction
  return 6 * NativeInstruction::instruction_size; // lui + addi + slli + addi + slli + jalr
}

void SharedRuntime::generate_trampoline(MacroAssembler *masm, address destination) {
  assert_cond(masm != NULL);
  int32_t offset = 0;
  __ movptr_with_offset(t0, destination, offset); // lui + addi + slli + addi + slli
  __ jalr(x0, t0, offset);
}

// The java_calling_convention describes stack locations as ideal slots on
// a frame with no abi restrictions. Since we must observe abi restrictions
// (like the placement of the register window) the slots must be biased by
// the following value.
static int reg2offset_in(VMReg r) {
  // Account for saved fp and lr
  // This should really be in_preserve_stack_slots
  return (r->reg2stack() + 4) * VMRegImpl::stack_slot_size;
}

static int reg2offset_out(VMReg r) {
  return (r->reg2stack() + SharedRuntime::out_preserve_stack_slots()) * VMRegImpl::stack_slot_size;
}

// ---------------------------------------------------------------------------
// Read the array of BasicTypes from a signature, and compute where the
// arguments should go.  Values in the VMRegPair regs array refer to 4-byte
// quantities.  Values less than VMRegImpl::stack0 are registers, those above
// refer to 4-byte stack slots.  All stack slots are based off of the stack pointer
// as framesizes are fixed.
// VMRegImpl::stack0 refers to the first slot 0(sp).
// and VMRegImpl::stack0+1 refers to the memory word 4-byes higher.  Register
// up to RegisterImpl::number_of_registers) are the 64-bit
// integer registers.

// Note: the INPUTS in sig_bt are in units of Java argument words,
// which are 64-bit.  The OUTPUTS are in 32-bit units.

// The Java calling convention is a "shifted" version of the C ABI.
// By skipping the first C ABI register we can call non-static jni
// methods with small numbers of arguments without having to shuffle
// the arguments at all. Since we control the java ABI we ought to at
// least get some advantage out of it.

int SharedRuntime::java_calling_convention(const BasicType *sig_bt,
                                           VMRegPair *regs,
                                           int total_args_passed,
                                           int is_outgoing) {
  assert_cond(sig_bt != NULL && regs != NULL);
  // Create the mapping between argument positions and
  // registers.
  static const Register INT_ArgReg[Argument::n_int_register_parameters_j] = {
    j_rarg0, j_rarg1, j_rarg2, j_rarg3,
    j_rarg4, j_rarg5, j_rarg6, j_rarg7
  };
  static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_j] = {
    j_farg0, j_farg1, j_farg2, j_farg3,
    j_farg4, j_farg5, j_farg6, j_farg7
  };

  uint int_args = 0;
  uint fp_args = 0;
  uint stk_args = 0; // inc by 2 each time

  for (int i = 0; i < total_args_passed; i++) {
    switch (sig_bt[i]) {
    case T_BOOLEAN:
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_VOID:
      // halves of T_LONG or T_DOUBLE
      assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
      regs[i].set_bad();
      break;
    case T_LONG:
      assert((i + 1) < total_args_passed && sig_bt[i + 1] == T_VOID, "expecting half");
      // fall through
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_FLOAT:
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_DOUBLE:
      assert((i + 1) < total_args_passed && sig_bt[i + 1] == T_VOID, "expecting half");
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    default:
      ShouldNotReachHere();
      break;
    }
  }

  return align_up(stk_args, 2);
}

// Patch the callers callsite with entry to compiled code if it exists.
static void patch_callers_callsite(MacroAssembler *masm) {
  assert_cond(masm != NULL);
  Label L;
  __ ld(t0, Address(xmethod, in_bytes(Method::code_offset())));
  __ beqz(t0, L);

  __ enter();
  __ push_CPU_state();

  // VM needs caller's callsite
  // VM needs target method
  // This needs to be a long call since we will relocate this adapter to
  // the codeBuffer and it may not reach

#ifndef PRODUCT
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif

  __ mv(c_rarg0, xmethod);
  __ mv(c_rarg1, lr);
  int32_t offset = 0;
  __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, SharedRuntime::fixup_callers_callsite)), offset);
  __ jalr(x1, t0, offset);
  __ ifence();
  __ pop_CPU_state();
  // restore sp
  __ leave();
  __ bind(L);
}

static void gen_c2i_adapter(MacroAssembler *masm,
                            int total_args_passed,
                            int comp_args_on_stack,
                            const BasicType *sig_bt,
                            const VMRegPair *regs,
                            Label& skip_fixup) {
  assert_cond(masm != NULL && sig_bt != NULL && regs != NULL);
  // Before we get into the guts of the C2I adapter, see if we should be here
  // at all.  We've come from compiled code and are attempting to jump to the
  // interpreter, which means the caller made a static call to get here
  // (vcalls always get a compiled target if there is one).  Check for a
  // compiled target.  If there is one, we need to patch the caller's call.
  patch_callers_callsite(masm);

  __ bind(skip_fixup);

  int words_pushed = 0;

  // Since all args are passed on the stack, total_args_passed *
  // Interpreter::stackElementSize is the space we need.

  int extraspace = total_args_passed * Interpreter::stackElementSize;

  __ mv(x30, sp);

  // stack is aligned, keep it that way
  extraspace = align_up(extraspace, 2 * wordSize);

  if (extraspace) {
    __ sub(sp, sp, extraspace);
  }

  // Now write the args into the outgoing interpreter space
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "missing half");
      continue;
    }

    // offset to start parameters
    int st_off   = (total_args_passed - i - 1) * Interpreter::stackElementSize;
    int next_off = st_off - Interpreter::stackElementSize;

    // Say 4 args:
    // i   st_off
    // 0   32 T_LONG
    // 1   24 T_VOID
    // 2   16 T_OBJECT
    // 3    8 T_BOOL
    // -    0 return address
    //
    // However to make thing extra confusing. Because we can fit a long/double in
    // a single slot on a 64 bt vm and it would be silly to break them up, the interpreter
    // leaves one slot empty and only stores to a single slot. In this case the
    // slot that is occupied is the T_VOID slot. See I said it was confusing.

    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // memory to memory use t0
      int ld_off = (r_1->reg2stack() * VMRegImpl::stack_slot_size
                    + extraspace
                    + words_pushed * wordSize);
      if (!r_2->is_valid()) {
        __ lwu(t0, Address(sp, ld_off));
        __ sd(t0, Address(sp, st_off), /*temp register*/esp);
      } else {
        __ ld(t0, Address(sp, ld_off), /*temp register*/esp);

        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if ( sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          // ld_off == LSW, ld_off+wordSize == MSW
          // st_off == MSW, next_off == LSW
          __ sd(t0, Address(sp, next_off), /*temp register*/esp);
#ifdef ASSERT
          // Overwrite the unused slot with known junk
          __ li(t0, 0xdeadffffdeadaaaaul);
          __ sd(t0, Address(sp, st_off), /*temp register*/esp);
#endif /* ASSERT */
        } else {
          __ sd(t0, Address(sp, st_off), /*temp register*/esp);
        }
      }
    } else if (r_1->is_Register()) {
      Register r = r_1->as_Register();
      if (!r_2->is_valid()) {
        // must be only an int (or less ) so move only 32bits to slot
        __ sd(r, Address(sp, st_off));
      } else {
        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if ( sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          // long/double in gpr
#ifdef ASSERT
          // Overwrite the unused slot with known junk
          __ li(t0, 0xdeadffffdeadaaabul);
          __ sd(t0, Address(sp, st_off), /*temp register*/esp);
#endif /* ASSERT */
          __ sd(r, Address(sp, next_off));
        } else {
          __ sd(r, Address(sp, st_off));
        }
      }
    } else {
      assert(r_1->is_FloatRegister(), "");
      if (!r_2->is_valid()) {
        // only a float use just part of the slot
        __ fsw(r_1->as_FloatRegister(), Address(sp, st_off));
      } else {
#ifdef ASSERT
        // Overwrite the unused slot with known junk
        __ li(t0, 0xdeadffffdeadaaacul);
        __ sd(t0, Address(sp, st_off), /*temp register*/esp);
#endif /* ASSERT */
        __ fsd(r_1->as_FloatRegister(), Address(sp, next_off));
      }
    }
  }

  __ mv(esp, sp); // Interp expects args on caller's expression stack

  __ ld(t0, Address(xmethod, in_bytes(Method::interpreter_entry_offset())));
  __ jr(t0);
}

void SharedRuntime::gen_i2c_adapter(MacroAssembler *masm,
                                    int total_args_passed,
                                    int comp_args_on_stack,
                                    const BasicType *sig_bt,
                                    const VMRegPair *regs) {
  // Cut-out for having no stack args.
  assert_cond(masm != NULL && sig_bt != NULL && regs != NULL);
  int comp_words_on_stack = align_up(comp_args_on_stack * VMRegImpl::stack_slot_size, wordSize) >> LogBytesPerWord;
  if (comp_args_on_stack != 0) {
    __ sub(t0, sp, comp_words_on_stack * wordSize);
    __ andi(sp, t0, -16);
  }

  // Will jump to the compiled code just as if compiled code was doing it.
  // Pre-load the register-jump target early, to schedule it better.
  __ ld(t1, Address(xmethod, in_bytes(Method::from_compiled_offset())));

  // Now generate the shuffle code.
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "missing half");
      continue;
    }

    // Pick up 0, 1 or 2 words from SP+offset.

    assert(!regs[i].second()->is_valid() || regs[i].first()->next() == regs[i].second(),
           "scrambled load targets?");
    // Load in argument order going down.
    int ld_off = (total_args_passed - i - 1) * Interpreter::stackElementSize;
    // Point to interpreter value (vs. tag)
    int next_off = ld_off - Interpreter::stackElementSize;

    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // Convert stack slot to an SP offset (+ wordSize to account for return address )
      int st_off = regs[i].first()->reg2stack() * VMRegImpl::stack_slot_size;
      if (!r_2->is_valid()) {
        __ lw(t0, Address(esp, ld_off));
        __ sd(t0, Address(sp, st_off), /*temp register*/t2);
      } else {
        //
        // We are using two optoregs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.
        //
        // Interpreter local[n] == MSW, local[n+1] == LSW however locals
        // are accessed as negative so LSW is at LOW address

        // ld_off is MSW so get LSW
        const int offset = (sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) ?
                           next_off : ld_off;
        __ ld(t0, Address(esp, offset));
        // st_off is LSW (i.e. reg.first())
        __ sd(t0, Address(sp, st_off), /*temp register*/t2);
      }
    } else if (r_1->is_Register()) {  // Register argument
      Register r = r_1->as_Register();
      if (r_2->is_valid()) {
        //
        // We are using two VMRegs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.

        const int offset = (sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) ?
                           next_off : ld_off;

        // this can be a misaligned move
        __ ld(r, Address(esp, offset));
      } else {
        // sign extend and use a full word?
        __ lw(r, Address(esp, ld_off));
      }
    } else {
      if (!r_2->is_valid()) {
        __ flw(r_1->as_FloatRegister(), Address(esp, ld_off));
      } else {
        __ fld(r_1->as_FloatRegister(), Address(esp, next_off));
      }
    }
  }

  // 6243940 We might end up in handle_wrong_method if
  // the callee is deoptimized as we race thru here. If that
  // happens we don't want to take a safepoint because the
  // caller frame will look interpreted and arguments are now
  // "compiled" so it is much better to make this transition
  // invisible to the stack walking code. Unfortunately if
  // we try and find the callee by normal means a safepoint
  // is possible. So we stash the desired callee in the thread
  // and the vm will find there should this case occur.

  __ sd(xmethod, Address(xthread, JavaThread::callee_target_offset()));

  __ jr(t1);
}

// ---------------------------------------------------------------
AdapterHandlerEntry* SharedRuntime::generate_i2c2i_adapters(MacroAssembler *masm,
                                                            int total_args_passed,
                                                            int comp_args_on_stack,
                                                            const BasicType *sig_bt,
                                                            const VMRegPair *regs,
                                                            AdapterFingerPrint* fingerprint) {
  assert_cond(masm != NULL && sig_bt != NULL && regs != NULL && fingerprint != NULL);
  address i2c_entry = __ pc();
  gen_i2c_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs);

  address c2i_unverified_entry = __ pc();
  Label skip_fixup;

  Label ok;

  const Register holder = t1;
  const Register receiver = j_rarg0;
  const Register tmp = t2;  // A call-clobbered register not used for arg passing

  // -------------------------------------------------------------------------
  // Generate a C2I adapter.  On entry we know xmethod holds the Method* during calls
  // to the interpreter.  The args start out packed in the compiled layout.  They
  // need to be unpacked into the interpreter layout.  This will almost always
  // require some stack space.  We grow the current (compiled) stack, then repack
  // the args.  We  finally end in a jump to the generic interpreter entry point.
  // On exit from the interpreter, the interpreter will restore our SP (lest the
  // compiled code, which relys solely on SP and not FP, get sick).

  {
    __ block_comment("c2i_unverified_entry {");
    __ load_klass(t0, receiver);
    __ ld(tmp, Address(holder, CompiledICHolder::holder_klass_offset()));
    __ ld(xmethod, Address(holder, CompiledICHolder::holder_metadata_offset()));
    __ beq(t0, tmp, ok);
    __ far_jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));

    __ bind(ok);
    // Method might have been compiled since the call site was patched to
    // interpreted; if that is the case treat it as a miss so we can get
    // the call site corrected.
    __ ld(t0, Address(xmethod, in_bytes(Method::code_offset())));
    __ beqz(t0, skip_fixup);
    __ far_jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));
    __ block_comment("} c2i_unverified_entry");
  }

  address c2i_entry = __ pc();

  gen_c2i_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs, skip_fixup);

  __ flush();
  return AdapterHandlerLibrary::new_entry(fingerprint, i2c_entry, c2i_entry, c2i_unverified_entry);
}

int SharedRuntime::c_calling_convention(const BasicType *sig_bt,
                                         VMRegPair *regs,
                                         VMRegPair *regs2,
                                         int total_args_passed) {
  assert(regs2 == NULL, "not needed on riscv64");
  assert_cond(sig_bt != NULL && regs != NULL);

  // We return the amount of VMRegImpl stack slots we need to reserve for all
  // the arguments NOT counting out_preserve_stack_slots.

  static const Register INT_ArgReg[Argument::n_int_register_parameters_c] = {
    c_rarg0, c_rarg1, c_rarg2, c_rarg3,
    c_rarg4, c_rarg5,  c_rarg6,  c_rarg7
  };
  static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_c] = {
    c_farg0, c_farg1, c_farg2, c_farg3,
    c_farg4, c_farg5, c_farg6, c_farg7
  };

  uint int_args = 0;
  uint fp_args = 0;
  uint stk_args = 0; // inc by 2 each time

  for (int i = 0; i < total_args_passed; i++) {
    switch (sig_bt[i]) {
    case T_BOOLEAN:
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_LONG:
      assert((i + 1) < total_args_passed && sig_bt[i + 1] == T_VOID, "expecting half");
      // fall through
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
    case T_METADATA:
      if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_FLOAT:
      if (fp_args < Argument::n_float_register_parameters_c) {
        regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
      } else if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_DOUBLE:
      assert((i + 1) < total_args_passed && sig_bt[i + 1] == T_VOID, "expecting half");
      if (fp_args < Argument::n_float_register_parameters_c) {
        regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
      } else if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_VOID: // Halves of longs and doubles
      assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
      regs[i].set_bad();
      break;
    default:
      ShouldNotReachHere();
      break;
    }
  }

  return stk_args;
}

// On 64 bit we will store integer like items to the stack as
// 64 bits items (sparc abi) even though java would only store
// 32bits for a parameter. On 32bit it will simply be 32 bits
// So this routine will do 32->32 on 32bit and 32->64 on 64bit
static void move32_64(MacroAssembler* masm, VMRegPair src, VMRegPair dst) {
  assert_cond(masm != NULL);
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      // stack to stack
      __ ld(t0, Address(fp, reg2offset_in(src.first())));
      __ sd(t0, Address(sp, reg2offset_out(dst.first())));
    } else {
      // stack to reg
      __ lw(dst.first()->as_Register(), Address(fp, reg2offset_in(src.first())));
    }
  } else if (dst.first()->is_stack()) {
    // reg to stack
    __ sd(src.first()->as_Register(), Address(sp, reg2offset_out(dst.first())));
  } else {
    if (dst.first() != src.first()) {
      // 32bits extend sign
      __ addw(dst.first()->as_Register(), src.first()->as_Register(), zr);
    }
  }
}

// An oop arg. Must pass a handle not the oop itself
static void object_move(MacroAssembler* masm,
                        OopMap* map,
                        int oop_handle_offset,
                        int framesize_in_slots,
                        VMRegPair src,
                        VMRegPair dst,
                        bool is_receiver,
                        int* receiver_offset) {
  assert_cond(masm != NULL && map != NULL && receiver_offset != NULL);
  // must pass a handle. First figure out the location we use as a handle
  Register rHandle = dst.first()->is_stack() ? t1 : dst.first()->as_Register();

  // See if oop is NULL if it is we need no handle

  if (src.first()->is_stack()) {

    // Oop is already on the stack as an argument
    int offset_in_older_frame = src.first()->reg2stack() + SharedRuntime::out_preserve_stack_slots();
    map->set_oop(VMRegImpl::stack2reg(offset_in_older_frame + framesize_in_slots));
    if (is_receiver) {
      *receiver_offset = (offset_in_older_frame + framesize_in_slots) * VMRegImpl::stack_slot_size;
    }

    __ ld(t0, Address(fp, reg2offset_in(src.first())));
    __ la(rHandle, Address(fp, reg2offset_in(src.first())));
    // conditionally move a NULL
    Label notZero1;
    __ bnez(t0, notZero1);
    __ mv(rHandle, zr);
    __ bind(notZero1);
  } else {

    // Oop is in an a register we must store it to the space we reserve
    // on the stack for oop_handles and pass a handle if oop is non-NULL

    const Register rOop = src.first()->as_Register();
    int oop_slot = -1;
    if (rOop == j_rarg0) {
      oop_slot = 0;
    } else if (rOop == j_rarg1) {
      oop_slot = 1;
    } else if (rOop == j_rarg2) {
      oop_slot = 2;
    } else if (rOop == j_rarg3) {
      oop_slot = 3;
    } else if (rOop == j_rarg4) {
      oop_slot = 4;
    } else if (rOop == j_rarg5) {
      oop_slot = 5;
    } else if (rOop == j_rarg6) {
      oop_slot = 6;
    } else {
      assert(rOop == j_rarg7, "wrong register");
      oop_slot = 7;
    }

    oop_slot = oop_slot * VMRegImpl::slots_per_word + oop_handle_offset;
    int offset = oop_slot * VMRegImpl::stack_slot_size;

    map->set_oop(VMRegImpl::stack2reg(oop_slot));
    // Store oop in handle area, may be NULL
    __ sd(rOop, Address(sp, offset));
    if (is_receiver) {
      *receiver_offset = offset;
    }

    //rOop maybe the same as rHandle
    if (rOop == rHandle) {
      Label isZero;
      __ beqz(rOop, isZero);
      __ la(rHandle, Address(sp, offset));
      __ bind(isZero);
    } else {
      Label notZero2;
      __ la(rHandle, Address(sp, offset));
      __ bnez(rOop, notZero2);
      __ mv(rHandle, zr);
      __ bind(notZero2);
    }
  }

  // If arg is on the stack then place it otherwise it is already in correct reg.
  if (dst.first()->is_stack()) {
    __ sd(rHandle, Address(sp, reg2offset_out(dst.first())));
  }
}

// A float arg may have to do float reg int reg conversion
static void float_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) {
  assert(src.first()->is_stack() && dst.first()->is_stack() ||
         src.first()->is_reg() && dst.first()->is_reg() || src.first()->is_stack() && dst.first()->is_reg(), "Unexpected error");
  assert_cond(masm != NULL);
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      __ lwu(t0, Address(fp, reg2offset_in(src.first())));
      __ sw(t0, Address(sp, reg2offset_out(dst.first())));
    } else if (dst.first()->is_Register()) {
      __ lwu(dst.first()->as_Register(), Address(fp, reg2offset_in(src.first())));
    } else {
      ShouldNotReachHere();
    }
  } else if (src.first() != dst.first()) {
    if (src.is_single_phys_reg() && dst.is_single_phys_reg()) {
      __ fmv_s(dst.first()->as_FloatRegister(), src.first()->as_FloatRegister());
    } else {
      ShouldNotReachHere();
    }
  }
}

// A long move
static void long_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) {
  assert_cond(masm != NULL);
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      // stack to stack
      __ ld(t0, Address(fp, reg2offset_in(src.first())));
      __ sd(t0, Address(sp, reg2offset_out(dst.first())));
    } else {
      // stack to reg
      __ ld(dst.first()->as_Register(), Address(fp, reg2offset_in(src.first())));
    }
  } else if (dst.first()->is_stack()) {
    // reg to stack
    __ sd(src.first()->as_Register(), Address(sp, reg2offset_out(dst.first())));
  } else {
    if (dst.first() != src.first()) {
      __ mv(dst.first()->as_Register(), src.first()->as_Register());
    }
  }
}

// A double move
static void double_move(MacroAssembler* masm, VMRegPair src, VMRegPair dst) {
  assert(src.first()->is_stack() && dst.first()->is_stack() ||
         src.first()->is_reg() && dst.first()->is_reg() || src.first()->is_stack() && dst.first()->is_reg(), "Unexpected error");
  assert_cond(masm != NULL);
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      __ ld(t0, Address(fp, reg2offset_in(src.first())));
      __ sd(t0, Address(sp, reg2offset_out(dst.first())));
    } else if (dst.first()-> is_Register()) {
      __ ld(dst.first()->as_Register(), Address(fp, reg2offset_in(src.first())));
    } else {
      ShouldNotReachHere();
    }
  } else if (src.first() != dst.first()) {
    if (src.is_single_phys_reg() && dst.is_single_phys_reg()) {
      __ fmv_d(dst.first()->as_FloatRegister(), src.first()->as_FloatRegister());
    } else {
      ShouldNotReachHere();
    }
  }
}

void SharedRuntime::save_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) {
  assert_cond(masm != NULL);
  // We always ignore the frame_slots arg and just use the space just below frame pointer
  // which by this time is free to use
  switch (ret_type) {
  case T_FLOAT:
    __ fsw(f10, Address(fp, -wordSize));
    break;
  case T_DOUBLE:
    __ fsd(f10, Address(fp, -wordSize));
    break;
  case T_VOID:  break;
  default: {
    __ sd(x10, Address(fp, -wordSize));
    }
  }
}

void SharedRuntime::restore_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) {
  assert_cond(masm != NULL);
  // We always ignore the frame_slots arg and just use the space just below frame pointer
  // which by this time is free to use
  switch (ret_type) {
  case T_FLOAT:
    __ flw(f10, Address(fp, -wordSize));
    break;
  case T_DOUBLE:
    __ fld(f10, Address(fp, -wordSize));
    break;
  case T_VOID:  break;
  default: {
    __ ld(x10, Address(fp, -wordSize));
    }
  }
}

static void save_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) {
  assert_cond(masm != NULL && args != NULL);
  RegSet x;
  for ( int i = first_arg ; i < arg_count ; i++ ) {
    if (args[i].first()->is_Register()) {
      x = x + args[i].first()->as_Register();
    } else if (args[i].first()->is_FloatRegister()) {
      __ addi(sp, sp, -2 * wordSize);
      __ fsd(args[i].first()->as_FloatRegister(), Address(sp, 0));
    }
  }
  __ push_reg(x, sp);
}

static void restore_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) {
  assert_cond(masm != NULL && args != NULL);
  RegSet x;
  for ( int i = first_arg ; i < arg_count ; i++ ) {
    if (args[i].first()->is_Register()) {
      x = x + args[i].first()->as_Register();
    } else {
      ;
    }
  }
  __ pop_reg(x, sp);
  for ( int i = arg_count - 1 ; i >= first_arg ; i-- ) {
    if (args[i].first()->is_Register()) {
      ;
    } else if (args[i].first()->is_FloatRegister()) {
      __ fld(args[i].first()->as_FloatRegister(), Address(sp, 0));
      __ add(sp, sp, 2 * wordSize);
    }
  }
}

// Check GCLocker::needs_gc and enter the runtime if it's true.  This
// keeps a new JNI critical region from starting until a GC has been
// forced.  Save down any oops in registers and describe them in an
// OopMap.
static void check_needs_gc_for_critical_native(MacroAssembler* masm,
                                               int stack_slots,
                                               int total_c_args,
                                               int total_in_args,
                                               int arg_save_area,
                                               OopMapSet* oop_maps,
                                               VMRegPair* in_regs,
                                               BasicType* in_sig_bt) { Unimplemented(); }

// Unpack an array argument into a pointer to the body and the length
// if the array is non-null, otherwise pass 0 for both.
static void unpack_array_argument(MacroAssembler* masm, VMRegPair reg, BasicType in_elem_type, VMRegPair body_arg, VMRegPair length_arg) { Unimplemented(); }

class ComputeMoveOrder: public StackObj {
  class MoveOperation: public ResourceObj {
    friend class ComputeMoveOrder;
   private:
    VMRegPair        _src;
    VMRegPair        _dst;
    int              _src_index;
    int              _dst_index;
    bool             _processed;
    MoveOperation*   _next;
    MoveOperation*   _prev;

    static int get_id(VMRegPair r) { Unimplemented(); return 0; }

   public:
    MoveOperation(int src_index, VMRegPair src, int dst_index, VMRegPair dst):
      _src(src)
    , _src_index(src_index)
    , _dst(dst)
    , _dst_index(dst_index)
    , _next(NULL)
    , _prev(NULL)
    , _processed(false) { Unimplemented(); }

    ~MoveOperation() {
      _next = NULL;
      _prev = NULL;
    }

    VMRegPair src() const              { Unimplemented(); return _src; }
    int src_id() const                 { Unimplemented(); return 0; }
    int src_index() const              { Unimplemented(); return 0; }
    VMRegPair dst() const              { Unimplemented(); return _src; }
    void set_dst(int i, VMRegPair dst) { Unimplemented(); }
    int dst_index() const              { Unimplemented(); return 0; }
    int dst_id() const                 { Unimplemented(); return 0; }
    MoveOperation* next() const        { Unimplemented(); return 0; }
    MoveOperation* prev() const        { Unimplemented(); return 0; }
    void set_processed()               { Unimplemented(); }
    bool is_processed() const          { Unimplemented(); return 0; }

    // insert
    void break_cycle(VMRegPair temp_register) { Unimplemented(); }

    void link(GrowableArray<MoveOperation*>& killer) { Unimplemented(); }
  };

 private:
  GrowableArray<MoveOperation*> edges;

 public:
  ComputeMoveOrder(int total_in_args, VMRegPair* in_regs, int total_c_args, VMRegPair* out_regs,
                   BasicType* in_sig_bt, GrowableArray<int>& arg_order, VMRegPair tmp_vmreg) { Unimplemented(); }

  ~ComputeMoveOrder() {}
  // Collected all the move operations
  void add_edge(int src_index, VMRegPair src, int dst_index, VMRegPair dst) { Unimplemented(); }

  // Walk the edges breaking cycles between moves.  The result list
  // can be walked in order to produce the proper set of loads
  GrowableArray<MoveOperation*>* get_store_order(VMRegPair temp_register) { Unimplemented(); return 0; }
};

static void rt_call(MacroAssembler* masm, address dest, int gpargs, int fpargs, int type) {
  assert_cond(masm != NULL);
  CodeBlob *cb = CodeCache::find_blob(dest);
  if (cb) {
    __ far_call(RuntimeAddress(dest));
  } else {
    assert((unsigned)gpargs < 256, "eek!");
    assert((unsigned)fpargs < 32, "eek!");
    int32_t offset = 0;
    __ la_patchable(t0, RuntimeAddress(dest), offset);
    __ jalr(x1, t0, offset);
    __ ifence();
  }
}

static void verify_oop_args(MacroAssembler* masm,
                            const methodHandle& method,
                            const BasicType* sig_bt,
                            const VMRegPair* regs) {
  assert_cond(masm != NULL && sig_bt != NULL && regs != NULL);
  const Register temp_reg = x9;  // not part of any compiled calling seq
  if (VerifyOops) {
    for (int i = 0; i < method->size_of_parameters(); i++) {
      if (sig_bt[i] == T_OBJECT ||
          sig_bt[i] == T_ARRAY) {
        VMReg r = regs[i].first();
        assert(r->is_valid(), "bad oop arg");
        if (r->is_stack()) {
          __ ld(temp_reg, Address(sp, r->reg2stack() * VMRegImpl::stack_slot_size));
          __ verify_oop(temp_reg);
        } else {
          __ verify_oop(r->as_Register());
        }
      }
    }
  }
}

static void gen_special_dispatch(MacroAssembler* masm,
                                 const methodHandle& method,
                                 const BasicType* sig_bt,
                                 const VMRegPair* regs) {
  assert_cond(masm != NULL && sig_bt != NULL && regs != NULL);
  verify_oop_args(masm, method, sig_bt, regs);
  vmIntrinsics::ID iid = method->intrinsic_id();

  // Now write the args into the outgoing interpreter space
  bool     has_receiver   = false;
  Register receiver_reg   = noreg;
  int      member_arg_pos = -1;
  Register member_reg     = noreg;
  int      ref_kind       = MethodHandles::signature_polymorphic_intrinsic_ref_kind(iid);
  if (ref_kind != 0) {
    member_arg_pos = method->size_of_parameters() - 1;  // trailing MemberName argument
    member_reg = x9;  // known to be free at this point
    has_receiver = MethodHandles::ref_kind_has_receiver(ref_kind);
  } else if (iid == vmIntrinsics::_invokeBasic) {
    has_receiver = true;
  } else {
    fatal("unexpected intrinsic id %d", iid);
  }

  if (member_reg != noreg) {
    // Load the member_arg into register, if necessary.
    SharedRuntime::check_member_name_argument_is_last_argument(method, sig_bt, regs);
    VMReg r = regs[member_arg_pos].first();
    if (r->is_stack()) {
      __ ld(member_reg, Address(sp, r->reg2stack() * VMRegImpl::stack_slot_size));
    } else {
      // no data motion is needed
      member_reg = r->as_Register();
    }
  }

  if (has_receiver) {
    // Make sure the receiver is loaded into a register.
    assert(method->size_of_parameters() > 0, "oob");
    assert(sig_bt[0] == T_OBJECT, "receiver argument must be an object");
    VMReg r = regs[0].first();
    assert(r->is_valid(), "bad receiver arg");
    if (r->is_stack()) {
      // Porting note:  This assumes that compiled calling conventions always
      // pass the receiver oop in a register.  If this is not true on some
      // platform, pick a temp and load the receiver from stack.
      fatal("receiver always in a register");
      receiver_reg = x12;  // known to be free at this point
      __ ld(receiver_reg, Address(sp, r->reg2stack() * VMRegImpl::stack_slot_size));
    } else {
      // no data motion is needed
      receiver_reg = r->as_Register();
    }
  }

  // Figure out which address we are really jumping to:
  MethodHandles::generate_method_handle_dispatch(masm, iid,
                                                 receiver_reg, member_reg, /*for_compiler_entry:*/ true);
}

// ---------------------------------------------------------------------------
// Generate a native wrapper for a given method.  The method takes arguments
// in the Java compiled code convention, marshals them to the native
// convention (handlizes oops, etc), transitions to native, makes the call,
// returns to java state (possibly blocking), unhandlizes any result and
// returns.
//
// Critical native functions are a shorthand for the use of
// GetPrimtiveArrayCritical and disallow the use of any other JNI
// functions.  The wrapper is expected to unpack the arguments before
// passing them to the callee and perform checks before and after the
// native call to ensure that they GCLocker
// lock_critical/unlock_critical semantics are followed.  Some other
// parts of JNI setup are skipped like the tear down of the JNI handle
// block and the check for pending exceptions it's impossible for them
// to be thrown.
//
// They are roughly structured like this:
//    if (GCLocker::needs_gc()) SharedRuntime::block_for_jni_critical()
//    tranistion to thread_in_native
//    unpack arrray arguments and call native entry point
//    check for safepoint in progress
//    check if any thread suspend flags are set
//      call into JVM and possible unlock the JNI critical
//      if a GC was suppressed while in the critical native.
//    transition back to thread_in_Java
//    return to caller
//
nmethod* SharedRuntime::generate_native_wrapper(MacroAssembler* masm,
                                                const methodHandle& method,
                                                int compile_id,
                                                BasicType* in_sig_bt,
                                                VMRegPair* in_regs,
                                                BasicType ret_type,
                                                address critical_entry) {
  assert_cond(masm != NULL && in_sig_bt != NULL && in_regs != NULL);
  if (method->is_method_handle_intrinsic()) {
    vmIntrinsics::ID iid = method->intrinsic_id();
    intptr_t start = (intptr_t)__ pc();
    int vep_offset = ((intptr_t)__ pc()) - start;

    // First instruction must be a nop as it may need to be patched on deoptimisation
    __ nop();
    gen_special_dispatch(masm,
                         method,
                         in_sig_bt,
                         in_regs);
    int frame_complete = ((intptr_t)__ pc()) - start;  // not complete, period
    __ flush();
    int stack_slots = SharedRuntime::out_preserve_stack_slots();  // no out slots at all, actually
    return nmethod::new_native_nmethod(method,
                                       compile_id,
                                       masm->code(),
                                       vep_offset,
                                       frame_complete,
                                       stack_slots / VMRegImpl::slots_per_word,
                                       in_ByteSize(-1),
                                       in_ByteSize(-1),
                                       (OopMapSet*)NULL);
  }
  bool is_critical_native = true;
  address native_func = critical_entry;
  if (native_func == NULL) {
    native_func = method->native_function();
    is_critical_native = false;
  }
  assert(native_func != NULL, "must have function");

  // An OopMap for lock (and class if static)
  OopMapSet *oop_maps = new OopMapSet();
  assert_cond(oop_maps != NULL);
  intptr_t start = (intptr_t)__ pc();

  // We have received a description of where all the java arg are located
  // on entry to the wrapper. We need to convert these args to where
  // the jni function will expect them. To figure out where they go
  // we convert the java signature to a C signature by inserting
  // the hidden arguments as arg[0] and possibly arg[1] (static method)

  const int total_in_args = method->size_of_parameters();
  int total_c_args = total_in_args;
  if (!is_critical_native) {
    total_c_args += 1;
    if (method->is_static()) {
      total_c_args++;
    }
  } else {
    for (int i = 0; i < total_in_args; i++) {
      if (in_sig_bt[i] == T_ARRAY) {
        total_c_args++;
      }
    }
  }

  BasicType* out_sig_bt = NEW_RESOURCE_ARRAY(BasicType, total_c_args);
  VMRegPair* out_regs   = NEW_RESOURCE_ARRAY(VMRegPair, total_c_args);
  assert_cond(out_sig_bt != NULL && out_regs != NULL);
  BasicType* in_elem_bt = NULL;

  int argc = 0;
  if (!is_critical_native) {
    out_sig_bt[argc++] = T_ADDRESS;
    if (method->is_static()) {
      out_sig_bt[argc++] = T_OBJECT;
    }

    for (int i = 0; i < total_in_args ; i++) {
      out_sig_bt[argc++] = in_sig_bt[i];
    }
  } else {
    Thread* THREAD = Thread::current();
    in_elem_bt = NEW_RESOURCE_ARRAY(BasicType, total_in_args);
    assert_cond(in_elem_bt != NULL);
    SignatureStream ss(method->signature());
    for (int i = 0; i < total_in_args ; i++) {
      if (in_sig_bt[i] == T_ARRAY) {
        // Arrays are passed as int, elem* pair
        out_sig_bt[argc++] = T_INT;
        out_sig_bt[argc++] = T_ADDRESS;
        Symbol* atype = ss.as_symbol(CHECK_NULL);
        const char* at = atype->as_C_string();
        if (strlen(at) == 2) {
          assert(at[0] == '[', "must be");
          switch (at[1]) {
            case 'B': in_elem_bt[i]  = T_BYTE; break;
            case 'C': in_elem_bt[i]  = T_CHAR; break;
            case 'D': in_elem_bt[i]  = T_DOUBLE; break;
            case 'F': in_elem_bt[i]  = T_FLOAT; break;
            case 'I': in_elem_bt[i]  = T_INT; break;
            case 'J': in_elem_bt[i]  = T_LONG; break;
            case 'S': in_elem_bt[i]  = T_SHORT; break;
            case 'Z': in_elem_bt[i]  = T_BOOLEAN; break;
            default: ShouldNotReachHere();
          }
        }
      } else {
        out_sig_bt[argc++] = in_sig_bt[i];
        in_elem_bt[i] = T_VOID;
      }
      if (in_sig_bt[i] != T_VOID) {
        assert(in_sig_bt[i] == ss.type(), "must match");
        ss.next();
      }
    }
  }

  // Now figure out where the args must be stored and how much stack space
  // they require.
  int out_arg_slots = c_calling_convention(out_sig_bt, out_regs, NULL, total_c_args);

  // Compute framesize for the wrapper.  We need to handlize all oops in
  // incoming registers

  // Calculate the total number of stack slots we will need.

  // First count the abi requirement plus all of the outgoing args
  int stack_slots = SharedRuntime::out_preserve_stack_slots() + out_arg_slots;

  // Now the space for the inbound oop handle area
  int total_save_slots = 8 * VMRegImpl::slots_per_word;  // 8 arguments passed in registers
  if (is_critical_native) {
    // Critical natives may have to call out so they need a save area
    // for register arguments.
    int double_slots = 0;
    int single_slots = 0;
    for ( int i = 0; i < total_in_args; i++) {
      if (in_regs[i].first()->is_Register()) {
        const Register reg = in_regs[i].first()->as_Register();
        switch (in_sig_bt[i]) {
          case T_BOOLEAN:
          case T_BYTE:
          case T_SHORT:
          case T_CHAR:
          case T_INT:  single_slots++; break;
          case T_ARRAY:  // specific to LP64 (7145024)
          case T_LONG: double_slots++; break;
          default:  ShouldNotReachHere();
        }
      } else if (in_regs[i].first()->is_FloatRegister()) {
        ShouldNotReachHere();
      }
    }
    total_save_slots = double_slots * 2 + single_slots;
    // align the save area
    if (double_slots != 0) {
      stack_slots = align_up(stack_slots, 2);
    }
  }

  int oop_handle_offset = stack_slots;
  stack_slots += total_save_slots;

  // Now any space we need for handlizing a klass if static method

  int klass_slot_offset = 0;
  int klass_offset = -1;
  int lock_slot_offset = 0;
  bool is_static = false;

  if (method->is_static()) {
    klass_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
    klass_offset = klass_slot_offset * VMRegImpl::stack_slot_size;
    is_static = true;
  }

  // Plus a lock if needed

  if (method->is_synchronized()) {
    lock_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
  }

  // Now a place (+2) to save return values or temp during shuffling
  // + 4 for return address (which we own) and saved fp
  stack_slots += 6;

  // Ok The space we have allocated will look like:
  //
  //
  // FP-> |                     |
  //      |---------------------|
  //      | 2 slots for moves   |
  //      |---------------------|
  //      | lock box (if sync)  |
  //      |---------------------| <- lock_slot_offset
  //      | klass (if static)   |
  //      |---------------------| <- klass_slot_offset
  //      | oopHandle area      |
  //      |---------------------| <- oop_handle_offset (8 java arg registers)
  //      | outbound memory     |
  //      | based arguments     |
  //      |                     |
  //      |---------------------|
  //      |                     |
  // SP-> | out_preserved_slots |
  //
  //


  // Now compute actual number of stack words we need rounding to make
  // stack properly aligned.
  stack_slots = align_up(stack_slots, StackAlignmentInSlots);

  int stack_size = stack_slots * VMRegImpl::stack_slot_size;

  // First thing make an ic check to see if we should even be here

  // We are free to use all registers as temps without saving them and
  // restoring them except fp. fp is the only callee save register
  // as far as the interpreter and the compiler(s) are concerned.


  const Register ic_reg = t1;
  const Register receiver = j_rarg0;

  Label hit;
  Label exception_pending;

  assert_different_registers(ic_reg, receiver, t0);
  __ verify_oop(receiver);
  __ cmp_klass(receiver, ic_reg, t0, hit);

  __ far_jump(RuntimeAddress(SharedRuntime::get_ic_miss_stub()));

  // Verified entry point must be aligned
  __ align(8);

  __ bind(hit);

  int vep_offset = ((intptr_t)__ pc()) - start;

  // If we have to make this method not-entrant we'll overwrite its
  // first instruction with a jump.
  __ nop();

  // Generate stack overflow check
  if (UseStackBanging) {
    __ bang_stack_with_offset(JavaThread::stack_shadow_zone_size());
  } else {
    Unimplemented();
  }

  // Generate a new frame for the wrapper.
  __ enter();
  // -2 because return address is already present and so is saved fp
  __ sub(sp, sp, stack_size - 2 * wordSize);

  // Frame is now completed as far as size and linkage.
  int frame_complete = ((intptr_t)__ pc()) - start;

  // We use x18 as the oop handle for the receiver/klass
  // It is callee save so it survives the call to native

  const Register oop_handle_reg = x18;

  if (is_critical_native) {
    check_needs_gc_for_critical_native(masm, stack_slots, total_c_args, total_in_args,
                                       oop_handle_offset, oop_maps, in_regs, in_sig_bt);
  }

  //
  // We immediately shuffle the arguments so that any vm call we have to
  // make from here on out (sync slow path, jvmti, etc.) we will have
  // captured the oops from our caller and have a valid oopMap for
  // them.

  // -----------------
  // The Grand Shuffle

  // The Java calling convention is either equal (linux) or denser (win64) than the
  // c calling convention. However the because of the jni_env argument the c calling
  // convention always has at least one more (and two for static) arguments than Java.
  // Therefore if we move the args from java -> c backwards then we will never have
  // a register->register conflict and we don't have to build a dependency graph
  // and figure out how to break any cycles.
  //

  // Record esp-based slot for receiver on stack for non-static methods
  int receiver_offset = -1;

  // This is a trick. We double the stack slots so we can claim
  // the oops in the caller's frame. Since we are sure to have
  // more args than the caller doubling is enough to make
  // sure we can capture all the incoming oop args from the
  // caller.
  //
  OopMap* map = new OopMap(stack_slots * 2, 0 /* arg_slots*/);
  assert_cond(map != NULL);

  int float_args = 0;
  int int_args = 0;

#ifdef ASSERT
  bool reg_destroyed[RegisterImpl::number_of_registers];
  bool freg_destroyed[FloatRegisterImpl::number_of_registers];
  for ( int r = 0 ; r < RegisterImpl::number_of_registers ; r++ ) {
    reg_destroyed[r] = false;
  }
  for ( int f = 0 ; f < FloatRegisterImpl::number_of_registers ; f++ ) {
    freg_destroyed[f] = false;
  }

#endif /* ASSERT */

  // This may iterate in two different directions depending on the
  // kind of native it is.  The reason is that for regular JNI natives
  // the incoming and outgoing registers are offset upwards and for
  // critical natives they are offset down.
  GrowableArray<int> arg_order(2 * total_in_args);
  VMRegPair tmp_vmreg;
  tmp_vmreg.set2(x9->as_VMReg());

  if (!is_critical_native) {
    for (int i = total_in_args - 1, c_arg = total_c_args - 1; i >= 0; i--, c_arg--) {
      arg_order.push(i);
      arg_order.push(c_arg);
    }
  } else {
    // Compute a valid move order, using tmp_vmreg to break any cycles
    ComputeMoveOrder cmo(total_in_args, in_regs, total_c_args, out_regs, in_sig_bt, arg_order, tmp_vmreg);
  }

  int temploc = -1;
  for (int ai = 0; ai < arg_order.length(); ai += 2) {
    int i = arg_order.at(ai);
    int c_arg = arg_order.at(ai + 1);
    __ block_comment(err_msg("mv %d -> %d", i, c_arg));
    if (c_arg == -1) {
      assert(is_critical_native, "should only be required for critical natives");
      // This arg needs to be moved to a temporary
      __ mv(tmp_vmreg.first()->as_Register(), in_regs[i].first()->as_Register());
      in_regs[i] = tmp_vmreg;
      temploc = i;
      continue;
    } else if (i == -1) {
      assert(is_critical_native, "should only be required for critical natives");
      // Read from the temporary location
      assert(temploc != -1, "must be valid");
      i = temploc;
      temploc = -1;
    }
#ifdef ASSERT
    if (in_regs[i].first()->is_Register()) {
      assert(!reg_destroyed[in_regs[i].first()->as_Register()->encoding()], "destroyed reg!");
    } else if (in_regs[i].first()->is_FloatRegister()) {
      assert(!freg_destroyed[in_regs[i].first()->as_FloatRegister()->encoding()], "destroyed reg!");
    }
    if (out_regs[c_arg].first()->is_Register()) {
      reg_destroyed[out_regs[c_arg].first()->as_Register()->encoding()] = true;
    } else if (out_regs[c_arg].first()->is_FloatRegister()) {
      freg_destroyed[out_regs[c_arg].first()->as_FloatRegister()->encoding()] = true;
    }
#endif /* ASSERT */
    switch (in_sig_bt[i]) {
      case T_ARRAY:
        if (is_critical_native) {
          unpack_array_argument(masm, in_regs[i], in_elem_bt[i], out_regs[c_arg + 1], out_regs[c_arg]);
          c_arg++;
#ifdef ASSERT
          if (out_regs[c_arg].first()->is_Register()) {
            reg_destroyed[out_regs[c_arg].first()->as_Register()->encoding()] = true;
          } else if (out_regs[c_arg].first()->is_FloatRegister()) {
            freg_destroyed[out_regs[c_arg].first()->as_FloatRegister()->encoding()] = true;
          }
#endif
          int_args++;
          break;
        }
      // no break
      case T_OBJECT:
        assert(!is_critical_native, "no oop arguments");
        object_move(masm, map, oop_handle_offset, stack_slots, in_regs[i], out_regs[c_arg],
                    ((i == 0) && (!is_static)),
                    &receiver_offset);
        int_args++;
        break;
      case T_VOID:
        break;

      case T_FLOAT:
        float_move(masm, in_regs[i], out_regs[c_arg]);
        float_args++;
        break;

      case T_DOUBLE:
        assert( i + 1 < total_in_args &&
                in_sig_bt[i + 1] == T_VOID &&
                out_sig_bt[c_arg + 1] == T_VOID, "bad arg list");
        double_move(masm, in_regs[i], out_regs[c_arg]);
        float_args++;
        break;

      case T_LONG :
        long_move(masm, in_regs[i], out_regs[c_arg]);
        int_args++;
        break;

      case T_ADDRESS:
        assert(false, "found T_ADDRESS in java args");
        break;

      default:
        move32_64(masm, in_regs[i], out_regs[c_arg]);
        int_args++;
    }
  }

  // point c_arg at the first arg that is already loaded in case we
  // need to spill before we call out
  int c_arg = total_c_args - total_in_args;

  // Pre-load a static method's oop into c_rarg1.
  if (method->is_static() && !is_critical_native) {

    //  load oop into a register
    __ movoop(c_rarg1,
              JNIHandles::make_local(method->method_holder()->java_mirror()),
              /*immediate*/true);

    // Now handlize the static class mirror it's known not-null.
    __ sd(c_rarg1, Address(sp, klass_offset));
    map->set_oop(VMRegImpl::stack2reg(klass_slot_offset));

    // Now get the handle
    __ la(c_rarg1, Address(sp, klass_offset));
    // and protect the arg if we must spill
    c_arg--;
  }

  // Change state to native (we save the return address in the thread, since it might not
  // be pushed on the stack when we do a stack traversal).
  // We use the same pc/oopMap repeatedly when we call out

  Label native_return;
  __ set_last_Java_frame(sp, noreg, native_return, t0);

  Label dtrace_method_entry, dtrace_method_entry_done;
  {
    int32_t offset = 0;
    __ la_patchable(t0, ExternalAddress((address)&DTraceMethodProbes), offset);
    __ lbu(t0, Address(t0, offset));
    __ addw(t0, t0, zr);
    __ bnez(t0, dtrace_method_entry);
    __ bind(dtrace_method_entry_done);
  }

  // RedefineClasses() tracing support for obsolete method entry
  if (log_is_enabled(Trace, redefine, class, obsolete)) {
    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
      CAST_FROM_FN_PTR(address, SharedRuntime::rc_trace_method_entry),
      xthread, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
  }

  // Lock a synchronized method

  // Register definitions used by locking and unlocking

  const Register swap_reg = x10;
  const Register obj_reg  = x9;  // Will contain the oop
  const Register lock_reg = x30;  // Address of compiler lock object (BasicLock)
  const Register old_hdr  = x30;  // value of old header at unlock time
  const Register tmp      = lr;

  Label slow_path_lock;
  Label lock_done;

  if (method->is_synchronized()) {
    assert(!is_critical_native, "unhandled");

    const int mark_word_offset = BasicLock::displaced_header_offset_in_bytes();

    // Get the handle (the 2nd argument)
    __ mv(oop_handle_reg, c_rarg1);

    // Get address of the box

    __ la(lock_reg, Address(sp, lock_slot_offset * VMRegImpl::stack_slot_size));

    // Load the oop from the handle
    __ ld(obj_reg, Address(oop_handle_reg, 0));

    if (UseBiasedLocking) {
      __ biased_locking_enter(lock_reg, obj_reg, swap_reg, tmp, false, lock_done, &slow_path_lock);
    }

    // Load (object->mark() | 1) into swap_reg % x10
    __ ld(t0, Address(obj_reg, oopDesc::mark_offset_in_bytes()));
    __ ori(swap_reg, t0, 1);

    // Save (object->mark() | 1) into BasicLock's displaced header
    __ sd(swap_reg, Address(lock_reg, mark_word_offset));

    // src -> dest if dest == x10 else x10 <- dest
    {
      Label here;
      __ cmpxchg_obj_header(x10, lock_reg, obj_reg, t0, lock_done, /*fallthrough*/NULL);
    }

    // Test if the oopMark is an obvious stack pointer, i.e.,
    //  1) (mark & 3) == 0, and
    //  2) sp <= mark < mark + os::pagesize()
    // These 3 tests can be done by evaluating the following
    // expression: ((mark - sp) & (3 - os::vm_page_size())),
    // assuming both stack pointer and pagesize have their
    // least significant 2 bits clear.
    // NOTE: the oopMark is in swap_reg % 10 as the result of cmpxchg

    __ sub(swap_reg, swap_reg, sp);
    __ andi(swap_reg, swap_reg, 3 - os::vm_page_size());

    // Save the test result, for recursive case, the result is zero
    __ sd(swap_reg, Address(lock_reg, mark_word_offset));
    __ bnez(swap_reg, slow_path_lock);

    // Slow path will re-enter here

    __ bind(lock_done);
  }


  // Finally just about ready to make the JNI call

  // get JNIEnv* which is first argument to native
  if (!is_critical_native) {
    __ la(c_rarg0, Address(xthread, in_bytes(JavaThread::jni_environment_offset())));
  }

  // Now set thread in native
  __ la(t1, Address(xthread, JavaThread::thread_state_offset()));
  __ mv(t0, _thread_in_native);
  __ membar(MacroAssembler::LoadStore | MacroAssembler::StoreStore);
  __ sw(t0, Address(t1));

  {
    int return_type = 0;
    switch (ret_type) {
    case T_VOID: break;
      return_type = 0; break;
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
    case T_BOOLEAN:
    case T_LONG:
      return_type = 1; break;
    case T_ARRAY:
    case T_OBJECT:
      return_type = 1; break;
    case T_FLOAT:
      return_type = 2; break;
    case T_DOUBLE:
      return_type = 3; break;
    default:
      ShouldNotReachHere();
    }
    rt_call(masm, native_func,
            int_args + 2, // riscv64 passes up to 8 args in int registers
            float_args,   // and up to 8 float args
            return_type);
  }

  __ bind(native_return);

  intptr_t return_pc = (intptr_t) __ pc();
  oop_maps->add_gc_map(return_pc - start, map);

  // Unpack native results.
  if(ret_type != T_OBJECT && ret_type != T_ARRAY) {
    __ cast_primitive_type(ret_type, x10);
  }

  // Switch thread to "native transition" state before reading the synchronization state.
  // This additional state is necessary because reading and testing the synchronization
  // state is not atomic w.r.t. GC, as this scenario demonstrates:
  //     Java thread A, in _thread_in_native state, loads _not_synchronized and is preempted.
  //     VM thread changes sync state to synchronizing and suspends threads for GC.
  //     Thread A is resumed to finish this native method, but doesn't block here since it
  //     didn't see any synchronization is progress, and escapes.
  __ mv(t0, _thread_in_native_trans);

  if(os::is_MP()) {
    if (UseMembar) {
      __ sw(t0, Address(xthread, JavaThread::thread_state_offset()));

      // Force this write out before the read below
      __ membar(MacroAssembler::AnyAny);
    } else {
      __ la(t1, Address(xthread, JavaThread::thread_state_offset()));
      __ membar(MacroAssembler::LoadStore | MacroAssembler::StoreStore);
      __ sw(t0, Address(t1));

      // Write serialization page so VM thread can do a pseudo remote membar.
      // We use the current thread pointer to calculate a thread specific
      // offset to write to within the page. This minimizes bus traffic
      // due to cache line collision.
      __ serialize_memory(xthread, x12, t0);
    }
  } else {
    __ sw(t0, Address(xthread, JavaThread::thread_state_offset()));
  }

  // check for safepoint operation in progress and/or pending suspend requests
  Label safepoint_in_progress, safepoint_in_progress_done;
  {
    __ safepoint_poll_acquire(safepoint_in_progress);
    __ lwu(t0, Address(xthread, JavaThread::suspend_flags_offset()));
    __ bnez(t0, safepoint_in_progress);
    __ bind(safepoint_in_progress_done);
  }

  // change thread state
  Label after_transition;
  __ la(t1, Address(xthread, JavaThread::thread_state_offset()));
  __ mv(t0, _thread_in_Java);
  __ membar(MacroAssembler::LoadStore | MacroAssembler::StoreStore);
  __ sw(t0, Address(t1));
  __ bind(after_transition);

  Label reguard;
  Label reguard_done;
  __ lbu(t0, Address(xthread, JavaThread::stack_guard_state_offset()));
  __ mv(t1, JavaThread::stack_guard_yellow_reserved_disabled);
  __ beq(t0, t1, reguard);
  __ bind(reguard_done);

  // native result if any is live

  // Unlock
  Label unlock_done;
  Label slow_path_unlock;
  if (method->is_synchronized()) {

    // Get locked oop from the handle we passed to jni
    __ ld(obj_reg, Address(oop_handle_reg, 0));

    Label done;

    if (UseBiasedLocking) {
      __ biased_locking_exit(obj_reg, old_hdr, done);
    }

    // Simple recursive lock?

    __ ld(t0, Address(sp, lock_slot_offset * VMRegImpl::stack_slot_size));
    __ beqz(t0, done);

    // Must save x10 if if it is live now because cmpxchg must use it
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      save_native_result(masm, ret_type, stack_slots);
    }


    // get address of the stack lock
    __ la(x10, Address(sp, lock_slot_offset * VMRegImpl::stack_slot_size));
    //  get old displaced header
    __ ld(old_hdr, Address(x10, 0));

    // Atomic swap old header if oop still contains the stack lock
    Label succeed;
    __ cmpxchg_obj_header(x10, old_hdr, obj_reg, t0, succeed, &slow_path_unlock);
    __ bind(succeed);

    // slow path re-enters here
    __ bind(unlock_done);
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      restore_native_result(masm, ret_type, stack_slots);
    }

    __ bind(done);
  }

  Label dtrace_method_exit, dtrace_method_exit_done;
  {
    int32_t offset = 0;
    __ la_patchable(t0, ExternalAddress((address)&DTraceMethodProbes), offset);
    __ lbu(t0, Address(t0, offset));
    __ bnez(t0, dtrace_method_exit);
    __ bind(dtrace_method_exit_done);
  }

  __ reset_last_Java_frame(false);

  // Unbox oop result, e.g. JNIHandles::resolve result.
  if (ret_type == T_OBJECT || ret_type == T_ARRAY) {
    __ resolve_jobject(x10, xthread, t1);
  }

  if (CheckJNICalls) {
    // clear_pending_jni_exception_check
    __ sd(zr, Address(xthread, JavaThread::pending_jni_exception_check_fn_offset()));
  }

  if (!is_critical_native) {
    // reset handle block
    __ ld(x12, Address(xthread, JavaThread::active_handles_offset()));
    __ sd(zr, Address(x12, JNIHandleBlock::top_offset_in_bytes()));
  }

  __ leave();

  if (!is_critical_native) {
    // Any exception pending?
    __ ld(t0, Address(xthread, in_bytes(Thread::pending_exception_offset())));
    __ bnez(t0, exception_pending);
  }

  // We're done
  __ ret();

  // Unexpected paths are out of line and go here

  if (!is_critical_native) {
    // forward the exception
    __ bind(exception_pending);

    // and forward the exception
    __ far_jump(RuntimeAddress(StubRoutines::forward_exception_entry()));
  }

  // Slow path locking & unlocking
  if (method->is_synchronized()) {

    __ block_comment("Slow path lock {");
    __ bind(slow_path_lock);

    // has last_Java_frame setup. No exceptions so do vanilla call not call_VM
    // args are (oop obj, BasicLock* lock, JavaThread* thread)

    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);

    __ mv(c_rarg0, obj_reg);
    __ mv(c_rarg1, lock_reg);
    __ mv(c_rarg2, xthread);

    // Not a leaf but we have last_Java_frame setup as we want
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_locking_C), 3);
    restore_args(masm, total_c_args, c_arg, out_regs);

#ifdef ASSERT
    { Label L;
      __ ld(t0, Address(xthread, in_bytes(Thread::pending_exception_offset())));
      __ beqz(t0, L);
      __ stop("no pending exception allowed on exit from monitorenter");
      __ bind(L);
    }
#endif
    __ j(lock_done);

    __ block_comment("} Slow path lock");

    __ block_comment("Slow path unlock {");
    __ bind(slow_path_unlock);

    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      save_native_result(masm, ret_type, stack_slots);
    }

    __ mv(c_rarg2, xthread);
    __ la(c_rarg1, Address(sp, lock_slot_offset * VMRegImpl::stack_slot_size));
    __ mv(c_rarg0, obj_reg);

    // Save pending exception around call to VM (which contains an EXCEPTION_MARK)
    // NOTE that obj_reg == x9 currently
    __ ld(x9, Address(xthread, in_bytes(Thread::pending_exception_offset())));
    __ sd(zr, Address(xthread, in_bytes(Thread::pending_exception_offset())));

    rt_call(masm, CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_unlocking_C), 3, 0, 1);

#ifdef ASSERT
    {
      Label L;
      __ ld(t0, Address(xthread, in_bytes(Thread::pending_exception_offset())));
      __ beqz(t0, L);
      __ stop("no pending exception allowed on exit complete_monitor_unlocking_C");
      __ bind(L);
    }
#endif /* ASSERT */

    __ sd(x9, Address(xthread, in_bytes(Thread::pending_exception_offset())));

    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      restore_native_result(masm, ret_type, stack_slots);
    }
    __ j(unlock_done);

    __ block_comment("} Slow path unlock");

  } // synchronized

  // SLOW PATH Reguard the stack if needed

  __ bind(reguard);
  save_native_result(masm, ret_type, stack_slots);
  rt_call(masm, CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages), 0, 0, 0);
  restore_native_result(masm, ret_type, stack_slots);
  // and continue
  __ j(reguard_done);

  // SLOW PATH safepoint
  {
    __ block_comment("safepoint {");
    __ bind(safepoint_in_progress);

    // Don't use call_VM as it will see a possible pending exception and forward it
    // and never return here preventing us from clearing _last_native_pc down below.
    //
    save_native_result(masm, ret_type, stack_slots);
    __ mv(c_rarg0, xthread);
#ifndef PRODUCT
    assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
    int32_t offset = 0;
    if (!is_critical_native) {
      __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans)), offset);
    } else {
      __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans_and_transition)), offset);
    }
    __ jalr(x1, t0, offset);
    __ ifence();
    // Restore any method result value
    restore_native_result(masm, ret_type, stack_slots);

    if (is_critical_native) {
      // The call above performed the transition to thread_in_Java so
      // skip the transition logic above.
      __ j(after_transition);
    }

    __ j(safepoint_in_progress_done);
    __ block_comment("} safepoint");
  }

  // SLOW PATH dtrace support
  {
    __ block_comment("dtrace entry {");
    __ bind(dtrace_method_entry);

    // We have all of the arguments setup at this point. We must not touch any register
    // argument registers at this point (what if we save/restore them there are no oop?

    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
      CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry),
      xthread, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
    __ j(dtrace_method_entry_done);
    __ block_comment("} dtrace entry");
  }

  {
    __ block_comment("dtrace exit {");
    __ bind(dtrace_method_exit);
    save_native_result(masm, ret_type, stack_slots);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
         CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit),
         xthread, c_rarg1);
    restore_native_result(masm, ret_type, stack_slots);
    __ j(dtrace_method_exit_done);
    __ block_comment("} dtrace exit");
  }

  __ flush();

  nmethod *nm = nmethod::new_native_nmethod(method,
                                            compile_id,
                                            masm->code(),
                                            vep_offset,
                                            frame_complete,
                                            stack_slots / VMRegImpl::slots_per_word,
                                            (is_static ? in_ByteSize(klass_offset) : in_ByteSize(receiver_offset)),
                                            in_ByteSize(lock_slot_offset*VMRegImpl::stack_slot_size),
                                            oop_maps);
  assert(nm != NULL, "create native nmethod fail!");
  if (is_critical_native) {
    nm->set_lazy_critical_native(true);
  }

  return nm;
}

// this function returns the adjust size (in number of words) to a c2i adapter
// activation for use during deoptimization
int Deoptimization::last_frame_adjust(int callee_parameters, int callee_locals) {
  assert(callee_locals >= callee_parameters,
         "test and remove; got more parms than locals");
  if (callee_locals < callee_parameters) {
    return 0;                   // No adjustment for negative locals
  }
  int diff = (callee_locals - callee_parameters) * Interpreter::stackElementWords;
  // diff is counted in stack words
  return align_up(diff, 2);
}

//------------------------------generate_deopt_blob----------------------------
void SharedRuntime::generate_deopt_blob() {
  // Allocate space for the code
  ResourceMark rm;
  // Setup code generation tools
  int pad = 0;
  CodeBuffer buffer("deopt_blob", 2048 + pad, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  int frame_size_in_words = -1;
  OopMap* map = NULL;
  OopMapSet *oop_maps = new OopMapSet();
  assert_cond(masm != NULL && oop_maps != NULL);

  // -------------
  // This code enters when returning to a de-optimized nmethod.  A return
  // address has been pushed on the the stack, and return values are in
  // registers.
  // If we are doing a normal deopt then we were called from the patched
  // nmethod from the point we returned to the nmethod. So the return
  // address on the stack is wrong by NativeCall::instruction_size
  // We will adjust the value so it looks like we have the original return
  // address on the stack (like when we eagerly deoptimized).
  // In the case of an exception pending when deoptimizing, we enter
  // with a return address on the stack that points after the call we patched
  // into the exception handler. We have the following register state from,
  // e.g., the forward exception stub (see stubGenerator_riscv64.cpp).
  //    x10: exception oop
  //    x9: exception handler
  //    x13: throwing pc
  // So in this case we simply jam x13 into the useless return address and
  // the stack looks just like we want.
  //
  // At this point we need to de-opt.  We save the argument return
  // registers.  We call the first C routine, fetch_unroll_info().  This
  // routine captures the return values and returns a structure which
  // describes the current frame size and the sizes of all replacement frames.
  // The current frame is compiled code and may contain many inlined
  // functions, each with their own JVM state.  We pop the current frame, then
  // push all the new frames.  Then we call the C routine unpack_frames() to
  // populate these frames.  Finally unpack_frames() returns us the new target
  // address.  Notice that callee-save registers are BLOWN here; they have
  // already been captured in the vframeArray at the time the return PC was
  // patched.
  address start = __ pc();
  Label cont;

  // Prolog for non exception case!

  // Save everything in sight.
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // Normal deoptimization.  Save exec mode for unpack_frames.
  __ mvw(xcpool, Deoptimization::Unpack_deopt); // callee-saved
  __ j(cont);

  int reexecute_offset = __ pc() - start;

  // Reexecute case
  // return address is the pc describes what bci to do re-execute at

  // No need to update map as each call to save_live_registers will produce identical oopmap
  (void) RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  __ mvw(xcpool, Deoptimization::Unpack_reexecute); // callee-saved
  __ j(cont);

  int exception_offset = __ pc() - start;

  // Prolog for exception case

  // all registers are dead at this entry point, except for x10, and
  // x13 which contain the exception oop and exception pc
  // respectively.  Set them in TLS and fall thru to the
  // unpack_with_exception_in_tls entry point.

  __ sd(x13, Address(xthread, JavaThread::exception_pc_offset()));
  __ sd(x10, Address(xthread, JavaThread::exception_oop_offset()));

  int exception_in_tls_offset = __ pc() - start;

  // new implementation because exception oop is now passed in JavaThread

  // Prolog for exception case
  // All registers must be preserved because they might be used by LinearScan
  // Exceptiop oop and throwing PC are passed in JavaThread
  // tos: stack at point of call to method that threw the exception (i.e. only
  // args are on the stack, no return address)

  // The return address pushed by save_live_registers will be patched
  // later with the throwing pc. The correct value is not available
  // now because loading it from memory would destroy registers.

  // NB: The SP at this point must be the SP of the method that is
  // being deoptimized.  Deoptimization assumes that the frame created
  // here by save_live_registers is immediately below the method's SP.
  // This is a somewhat fragile mechanism.

  // Save everything in sight.
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // Now it is safe to overwrite any register

  // Deopt during an exception.  Save exec mode for unpack_frames.
  __ li(xcpool, Deoptimization::Unpack_exception); // callee-saved

  // load throwing pc from JavaThread and patch it as the return address
  // of the current frame. Then clear the field in JavaThread

  __ ld(x13, Address(xthread, JavaThread::exception_pc_offset()));
  __ sd(x13, Address(fp, wordSize));
  __ sd(zr, Address(xthread, JavaThread::exception_pc_offset()));

#ifdef ASSERT
  // verify that there is really an exception oop in JavaThread
  __ ld(x10, Address(xthread, JavaThread::exception_oop_offset()));
  __ verify_oop(x10);

  // verify that there is no pending exception
  Label no_pending_exception;
  __ ld(t0, Address(xthread, Thread::pending_exception_offset()));
  __ beqz(t0, no_pending_exception);
  __ stop("must not have pending exception here");
  __ bind(no_pending_exception);
#endif

  __ bind(cont);

  // Call C code.  Need thread and this frame, but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.
  //
  // UnrollBlock* fetch_unroll_info(JavaThread* thread)

  // fetch_unroll_info needs to call last_java_frame().

  Label retaddr;
  __ set_last_Java_frame(sp, noreg, retaddr, t0);
#ifdef ASSERT
  {
    Label L;
    __ ld(t0, Address(xthread,
                              JavaThread::last_Java_fp_offset()));
    __ beqz(t0, L);
    __ stop("SharedRuntime::generate_deopt_blob: last_Java_fp not cleared");
    __ bind(L);
  }
#endif // ASSERT
  __ mv(c_rarg0, xthread);
  __ mv(c_rarg1, xcpool);
  int32_t offset = 0;
  __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, Deoptimization::fetch_unroll_info)), offset);
  __ jalr(x1, t0, offset);
  __ bind(retaddr);

  // Need to have an oopmap that tells fetch_unroll_info where to
  // find any register it might need.
  oop_maps->add_gc_map(__ pc() - start, map);

  __ reset_last_Java_frame(false);

  // Load UnrollBlock* into x15
  __ mv(x15, x10);

  __ lwu(xcpool, Address(x15, Deoptimization::UnrollBlock::unpack_kind_offset_in_bytes()));
  Label noException;
  __ li(t0, Deoptimization::Unpack_exception);
  __ bne(xcpool, t0, noException); // Was exception pending?
  __ ld(x10, Address(xthread, JavaThread::exception_oop_offset()));
  __ ld(x13, Address(xthread, JavaThread::exception_pc_offset()));
  __ sd(zr, Address(xthread, JavaThread::exception_oop_offset()));
  __ sd(zr, Address(xthread, JavaThread::exception_pc_offset()));

  __ verify_oop(x10);

  // Overwrite the result registers with the exception results.
  __ sd(x10, Address(sp, RegisterSaver::x10_offset_in_bytes()));

  __ bind(noException);

  // Only register save data is on the stack.
  // Now restore the result registers.  Everything else is either dead
  // or captured in the vframeArray.
  RegisterSaver::restore_result_registers(masm);

  // All of the register save area has been popped of the stack. Only the
  // return address remains.

  // Pop all the frames we must move/replace.
  //
  // Frame picture (youngest to oldest)
  // 1: self-frame (no frame link)
  // 2: deopting frame  (no frame link)
  // 3: caller of deopting frame (could be compiled/interpreted).
  //
  // Note: by leaving the return address of self-frame on the stack
  // and using the size of frame 2 to adjust the stack
  // when we are done the return to frame 3 will still be on the stack.

  // Pop deoptimized frame
  __ lwu(x12, Address(x15, Deoptimization::UnrollBlock::size_of_deoptimized_frame_offset_in_bytes()));
  __ sub(x12, x12, 2 * wordSize);
  __ add(sp, sp, x12);
  __ ld(fp, Address(sp, 0));
  __ ld(lr, Address(sp, wordSize));
  __ addi(sp, sp, 2 * wordSize);
  // LR should now be the return address to the caller (3)

#ifdef ASSERT
  // Compilers generate code that bang the stack by as much as the
  // interpreter would need. So this stack banging should never
  // trigger a fault. Verify that it does not on non product builds.
  if (UseStackBanging) {
    __ lwu(x9, Address(x15, Deoptimization::UnrollBlock::total_frame_sizes_offset_in_bytes()));
    __ bang_stack_size(x9, x12);
  }
#endif
  // Load address of array of frame pcs into x12
  __ ld(x12, Address(x15, Deoptimization::UnrollBlock::frame_pcs_offset_in_bytes()));

  // Load address of array of frame sizes into x14
  __ ld(x14, Address(x15, Deoptimization::UnrollBlock::frame_sizes_offset_in_bytes()));

  // Load counter into x13
  __ lwu(x13, Address(x15, Deoptimization::UnrollBlock::number_of_frames_offset_in_bytes()));

  // Now adjust the caller's stack to make up for the extra locals
  // but record the original sp so that we can save it in the skeletal interpreter
  // frame and the stack walking of interpreter_sender will get the unextended sp
  // value and not the "real" sp value.

  const Register sender_sp = x16;

  __ mv(sender_sp, sp);
  __ lwu(x9, Address(x15,
                     Deoptimization::UnrollBlock::
                     caller_adjustment_offset_in_bytes()));
  __ sub(sp, sp, x9);

  // Push interpreter frames in a loop
  __ li(t0, 0xDEADDEAD);               // Make a recognizable pattern
  __ mv(t1, t0);
  Label loop;
  __ bind(loop);
  __ ld(x9, Address(x14, 0));          // Load frame size
  __ addi(x14, x14, wordSize);
  __ sub(x9, x9, 2 * wordSize);        // We'll push pc and fp by hand
  __ ld(lr, Address(x12, 0));          // Load pc
  __ addi(x12, x12, wordSize);
  __ enter();                          // Save old & set new fp
  __ sub(sp, sp, x9);                  // Prolog
  // This value is corrected by layout_activation_impl
  __ sd(zr, Address(fp, frame::interpreter_frame_last_sp_offset * wordSize));
  __ sd(sender_sp, Address(fp, frame::interpreter_frame_sender_sp_offset * wordSize)); // Make it walkable
  __ mv(sender_sp, sp);                // Pass sender_sp to next frame
  __ addi(x13, x13, -1);               // Decrement counter
  __ bnez(x13, loop);

    // Re-push self-frame
  __ ld(lr, Address(x12));
  __ enter();

  // Allocate a full sized register save area.  We subtract 2 because
  // enter() just pushed 2 words
  __ sub(sp, sp, (frame_size_in_words - 2) * wordSize);

  // Restore frame locals after moving the frame
  __ fsd(f10, Address(sp, RegisterSaver::f10_offset_in_bytes()));
  __ sd(x10, Address(sp, RegisterSaver::x10_offset_in_bytes()));

  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // restore return values to their stack-slots with the new SP.
  //
  // void Deoptimization::unpack_frames(JavaThread* thread, int exec_mode)

  // Use fp because the frames look interpreted now
  // Don't need the precise return PC here, just precise enough to point into this code blob.
  address the_pc = __ pc();
  __ set_last_Java_frame(sp, fp, the_pc, t0);

  __ mv(c_rarg0, xthread);
  __ mv(c_rarg1, xcpool); // second arg: exec_mode
  offset = 0;
  __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, Deoptimization::unpack_frames)), offset);
  __ jalr(x1, t0, offset);

  // Set an oopmap for the call site
  // Use the same PC we used for the last java frame
  oop_maps->add_gc_map(the_pc - start,
                       new OopMap( frame_size_in_words, 0 ));

  // Clear fp AND pc
  __ reset_last_Java_frame(true);

  // Collect return values
  __ fld(f10, Address(sp, RegisterSaver::f10_offset_in_bytes()));
  __ ld(x10, Address(sp, RegisterSaver::x10_offset_in_bytes()));

  // Pop self-frame.
  __ leave();                           // Epilog

  // Jump to interpreter
  __ ret();

  // Make sure all code is generated
  masm->flush();

  _deopt_blob = DeoptimizationBlob::create(&buffer, oop_maps, 0, exception_offset, reexecute_offset, frame_size_in_words);
  assert(_deopt_blob != NULL, "create deoptimization blob fail!");
  _deopt_blob->set_unpack_with_exception_in_tls_offset(exception_in_tls_offset);
}

uint SharedRuntime::out_preserve_stack_slots() {
  return 0;
}

#ifdef COMPILER2
//------------------------------generate_uncommon_trap_blob--------------------
void SharedRuntime::generate_uncommon_trap_blob() {
  // Allocate space for the code
  ResourceMark rm;
  // Setup code generation tools
  CodeBuffer buffer("uncommon_trap_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  assert_cond(masm != NULL);

  assert(SimpleRuntimeFrame::framesize % 4 == 0, "sp not 16-byte aligned");

  address start = __ pc();

  // Push self-frame.  We get here with a return address in LR
  // and sp should be 16 byte aligned
  // push fp and retaddr by hand
  __ addi(sp, sp, -2 * wordSize);
  __ sd(lr, Address(sp, wordSize));
  __ sd(fp, Address(sp, 0));
  // we don't expect an arg reg save area
#ifndef PRODUCT
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
  // compiler left unloaded_class_index in j_rarg0 move to where the
  // runtime expects it.
  __ addiw(c_rarg1, j_rarg0, 0);

  // we need to set the past SP to the stack pointer of the stub frame
  // and the pc to the address where this runtime call will return
  // although actually any pc in this code blob will do).
  Label retaddr;
  __ set_last_Java_frame(sp, noreg, retaddr, t0);

  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // capture callee-saved registers as well as return values.
  //
  // UnrollBlock* uncommon_trap(JavaThread* thread, jint unloaded_class_index, jint exec_mode)
  //
  // n.b. 3 gp args, 0 fp args, integral return type

  __ mv(c_rarg0, xthread);
  __ mvw(c_rarg2, (unsigned)Deoptimization::Unpack_uncommon_trap);
  int32_t offset = 0;
  __ la_patchable(t0,
        RuntimeAddress(CAST_FROM_FN_PTR(address,
                                        Deoptimization::uncommon_trap)), offset);
  __ jalr(x1, t0, offset);
  __ bind(retaddr);

  // Set an oopmap for the call site
  OopMapSet* oop_maps = new OopMapSet();
  OopMap* map = new OopMap(SimpleRuntimeFrame::framesize, 0);
  assert_cond(oop_maps != NULL && map != NULL);

  // location of rfp is known implicitly by the frame sender code

  oop_maps->add_gc_map(__ pc() - start, map);

  __ reset_last_Java_frame(false);

  // move UnrollBlock* into x14
  __ mv(x14, x10);

#ifdef ASSERT
  { Label L;
    __ lwu(t0, Address(x14, Deoptimization::UnrollBlock::unpack_kind_offset_in_bytes()));
    __ mvw(t1, Deoptimization::Unpack_uncommon_trap);
    __ beq(t0, t1, L);
    __ stop("SharedRuntime::generate_deopt_blob: last_Java_fp not cleared");
    __ bind(L);
  }
#endif

  // Pop all the frames we must move/replace.
  //
  // Frame picture (youngest to oldest)
  // 1: self-frame (no frame link)
  // 2: deopting frame  (no frame link)
  // 3: caller of deopting frame (could be compiled/interpreted).

  __ add(sp, sp, (SimpleRuntimeFrame::framesize) << LogBytesPerInt); // Epilog!

  // Pop deoptimized frame (int)
  __ lwu(x12, Address(x14,
                      Deoptimization::UnrollBlock::
                      size_of_deoptimized_frame_offset_in_bytes()));
  __ sub(x12, x12, 2 * wordSize);
  __ add(sp, sp, x12);
  __ ld(fp, sp, 0);
  __ ld(lr, sp, wordSize);
  __ addi(sp, sp, 2 * wordSize);
  // LR should now be the return address to the caller (3) frame

#ifdef ASSERT
  // Compilers generate code that bang the stack by as much as the
  // interpreter would need. So this stack banging should never
  // trigger a fault. Verify that it does not on non product builds.
  if (UseStackBanging) {
    __ lwu(x11, Address(x14,
                        Deoptimization::UnrollBlock::
                        total_frame_sizes_offset_in_bytes()));
    __ bang_stack_size(x11, x12);
  }
#endif

  // Load address of array of frame pcs into x12 (address*)
  __ ld(x12, Address(x14,
                     Deoptimization::UnrollBlock::frame_pcs_offset_in_bytes()));

  // Load address of array of frame sizes into x15 (intptr_t*)
  __ ld(x15, Address(x14,
                     Deoptimization::UnrollBlock::
                     frame_sizes_offset_in_bytes()));

  // Counter
  __ lwu(x13, Address(x14,
                      Deoptimization::UnrollBlock::
                      number_of_frames_offset_in_bytes())); // (int)

  // Now adjust the caller's stack to make up for the extra locals but
  // record the original sp so that we can save it in the skeletal
  // interpreter frame and the stack walking of interpreter_sender
  // will get the unextended sp value and not the "real" sp value.

  const Register sender_sp = t1; // temporary register

  __ lwu(x11, Address(x14,
                      Deoptimization::UnrollBlock::
                      caller_adjustment_offset_in_bytes())); // (int)
  __ mv(sender_sp, sp);
  __ sub(sp, sp, x11);

  // Push interpreter frames in a loop
  Label loop;
  __ bind(loop);
  __ ld(x11, Address(x15, 0));       // Load frame size
  __ sub(x11, x11, 2 * wordSize);    // We'll push pc and fp by hand
  __ ld(lr, Address(x12, 0));        // Save return address
  __ enter();                        // and old fp & set new fp
  __ sub(sp, sp, x11);               // Prolog
  __ sd(sender_sp, Address(fp, frame::interpreter_frame_sender_sp_offset * wordSize)); // Make it walkable
  // This value is corrected by layout_activation_impl
  __ sd(zr, Address(fp, frame::interpreter_frame_last_sp_offset * wordSize));
  __ mv(sender_sp, sp);              // Pass sender_sp to next frame
  __ add(x15, x15, wordSize);        // Bump array pointer (sizes)
  __ add(x12, x12, wordSize);        // Bump array pointer (pcs)
  __ subw(x13, x13, 1);              // Decrement counter
  __ bgtz(x13, loop);
  __ ld(lr, Address(x12, 0));        // save final return address
  // Re-push self-frame
  __ enter();                        // & old fp & set new fp

  // Use fp because the frames look interpreted now
  // Save "the_pc" since it cannot easily be retrieved using the last_java_SP after we aligned SP.
  // Don't need the precise return PC here, just precise enough to point into this code blob.
  address the_pc = __ pc();
  __ set_last_Java_frame(sp, fp, the_pc, t0);

  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // restore return values to their stack-slots with the new SP.
  //
  // BasicType unpack_frames(JavaThread* thread, int exec_mode)
  //

  // n.b. 2 gp args, 0 fp args, integral return type

  // sp should already be aligned
  __ mv(c_rarg0, xthread);
  __ mvw(c_rarg1, (unsigned)Deoptimization::Unpack_uncommon_trap);
  offset = 0;
  __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, Deoptimization::unpack_frames)), offset);
  __ jalr(x1, t0, offset);

  // Set an oopmap for the call site
  // Use the same PC we used for the last java frame
  oop_maps->add_gc_map(the_pc - start, new OopMap(SimpleRuntimeFrame::framesize, 0));

  // Clear fp AND pc
  __ reset_last_Java_frame(true);

  // Pop self-frame.
  __ leave();                 // Epilog

  // Jump to interpreter
  __ ret();

  // Make sure all code is generated
  masm->flush();

  _uncommon_trap_blob =  UncommonTrapBlob::create(&buffer, oop_maps,
                                                  SimpleRuntimeFrame::framesize >> 1);
}
#endif // COMPILER2

//------------------------------generate_handler_blob------
//
// Generate a special Compile2Runtime blob that saves all registers,
// and setup oopmap.
//
SafepointBlob* SharedRuntime::generate_handler_blob(address call_ptr, int poll_type) {
  ResourceMark rm;
  OopMapSet *oop_maps = new OopMapSet();
  assert_cond(oop_maps != NULL);
  OopMap* map = NULL;

  // Allocate space for the code.  Setup code generation tools.
  CodeBuffer buffer("handler_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  assert_cond(masm != NULL);

  address start   = __ pc();
  address call_pc = NULL;
  int frame_size_in_words = -1;
  bool cause_return = (poll_type == POLL_AT_RETURN);

  // Save Integer and Float registers.
  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  // The following is basically a call_VM.  However, we need the precise
  // address of the call in order to generate an oopmap. Hence, we do all the
  // work outselves.

  Label retaddr;
  __ set_last_Java_frame(sp, noreg, retaddr, t0);

  // The return address must always be correct so that frame constructor never
  // sees an invalid pc.

  if (!cause_return) {
    // overwrite the return address pushed by save_live_registers
    // Additionally, x18 is a callee-saved register so we can look at
    // it later to determine if someone changed the return address for
    // us!
    __ ld(x18, Address(xthread, JavaThread::saved_exception_pc_offset()));
    __ sd(x18, Address(fp, wordSize));
  }

  // Do the call
  __ mv(c_rarg0, xthread);
  int32_t offset = 0;
  __ la_patchable(t0, RuntimeAddress(call_ptr), offset);
  __ jalr(x1, t0, offset);
  __ bind(retaddr);

  // Set an oopmap for the call site.  This oopmap will map all
  // oop-registers and debug-info registers as callee-saved.  This
  // will allow deoptimization at this safepoint to find all possible
  // debug-info recordings, as well as let GC find all oops.

  oop_maps->add_gc_map( __ pc() - start, map);

  Label noException;

  __ reset_last_Java_frame(false);

  __ fence_i();
  __ membar(MacroAssembler::LoadLoad | MacroAssembler::LoadStore);

  __ ld(t0, Address(xthread, Thread::pending_exception_offset()));
  __ beqz(t0, noException);

  // Exception pending

  RegisterSaver::restore_live_registers(masm);

  __ far_jump(RuntimeAddress(StubRoutines::forward_exception_entry()));

  // No exception case
  __ bind(noException);

  Label no_adjust, bail;
  if (SafepointMechanism::uses_thread_local_poll() && !cause_return) {
    // If our stashed return pc was modified by the runtime we avoid touching it
    __ ld(t0, Address(fp, wordSize));
    __ bne(x18, t0, no_adjust);

#ifdef ASSERT
    // Verify the correct encoding of the poll we're about to skip.
    // See NativeInstruction::is_lwu_to_zr()
    __ lwu(t0, Address(x18));
    __ andi(t1, t0, 0b0000011);
    __ mv(t2, 0b0000011);
    __ bne(t1, t2, bail); // 0-6:0b0000011
    __ srli(t1, t0, 7);
    __ andi(t1, t1, 0b00000);
    __ bnez(t1, bail);    // 7-11:0b00000
    __ srli(t1, t0, 12);
    __ andi(t1, t1, 0b110);
    __ mv(t2, 0b110);
    __ bne(t1, t2, bail); // 12-14:0b110
#endif
    // Adjust return pc forward to step over the safepoint poll instruction
    __ add(x18, x18, NativeInstruction::instruction_size);
    __ sd(x18, Address(fp, wordSize));
  }

  __ bind(no_adjust);
  // Normal exit, restore registers and exit.
  RegisterSaver::restore_live_registers(masm);

  __ ret();

#ifdef ASSERT
  __ bind(bail);
  __ stop("Attempting to adjust pc to skip safepoint poll but the return point is not what we expected");
#endif

  // Make sure all code is generated
  masm->flush();

  // Fill-out other meta info
  return SafepointBlob::create(&buffer, oop_maps, frame_size_in_words);
}

//
// generate_resolve_blob - call resolution (static/virtual/opt-virtual/ic-miss
//
// Generate a stub that calls into vm to find out the proper destination
// of a java call. All the argument registers are live at this point
// but since this is generic code we don't know what they are and the caller
// must do any gc of the args.
//
RuntimeStub* SharedRuntime::generate_resolve_blob(address destination, const char* name) {
  assert (StubRoutines::forward_exception_entry() != NULL, "must be generated before");

  // allocate space for the code
  ResourceMark rm;

  CodeBuffer buffer(name, 1000, 512);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  assert_cond(masm != NULL);

  int frame_size_in_words = -1;

  OopMapSet *oop_maps = new OopMapSet();
  assert_cond(oop_maps != NULL);
  OopMap* map = NULL;

  int start = __ offset();

  map = RegisterSaver::save_live_registers(masm, 0, &frame_size_in_words);

  int frame_complete = __ offset();

  {
    Label retaddr;
    __ set_last_Java_frame(sp, noreg, retaddr, t0);

    __ mv(c_rarg0, xthread);
    int32_t offset = 0;
    __ la_patchable(t0, RuntimeAddress(destination), offset);
    __ jalr(x1, t0, offset);
    __ bind(retaddr);
  }

  // Set an oopmap for the call site.
  // We need this not only for callee-saved registers, but also for volatile
  // registers that the compiler might be keeping live across a safepoint.

  oop_maps->add_gc_map( __ offset() - start, map);

  __ ifence();

  // x10 contains the address we are going to jump to assuming no exception got installed

  // clear last_Java_sp
  __ reset_last_Java_frame(false);
  // check for pending exceptions
  Label pending;
  __ ld(t0, Address(xthread, Thread::pending_exception_offset()));
  __ bnez(t0, pending);

  // get the returned Method*
  __ get_vm_result_2(xmethod, xthread);
  __ sd(xmethod, Address(sp, RegisterSaver::reg_offset_in_bytes(xmethod)));

  // x10 is where we want to jump, overwrite t0 which is saved and temporary
  __ sd(x10, Address(sp, RegisterSaver::tmp0_offset_in_bytes()));
  RegisterSaver::restore_live_registers(masm);

  // We are back the the original state on entry and ready to go.

  __ jr(t0);

  // Pending exception after the safepoint

  __ bind(pending);

  RegisterSaver::restore_live_registers(masm);

  // exception pending => remove activation and forward to exception handler

  __ sd(zr, Address(xthread, JavaThread::vm_result_offset()));

  __ ld(x10, Address(xthread, Thread::pending_exception_offset()));
  __ far_jump(RuntimeAddress(StubRoutines::forward_exception_entry()));

  // -------------
  // make sure all code is generated
  masm->flush();

  // return the  blob
  return RuntimeStub::new_runtime_stub(name, &buffer, frame_complete, frame_size_in_words, oop_maps, true);
}

#ifdef COMPILER2
// This is here instead of runtime_riscv64.cpp because it uses SimpleRuntimeFrame
//
//------------------------------generate_exception_blob---------------------------
// creates exception blob at the end
// Using exception blob, this code is jumped from a compiled method.
// (see emit_exception_handler in riscv64.ad file)
//
// Given an exception pc at a call we call into the runtime for the
// handler in this method. This handler might merely restore state
// (i.e. callee save registers) unwind the frame and jump to the
// exception handler for the nmethod if there is no Java level handler
// for the nmethod.
//
// This code is entered with a jmp.
//
// Arguments:
//   x10: exception oop
//   x13: exception pc
//
// Results:
//   x10: exception oop
//   x13: exception pc in caller
//   destination: exception handler of caller
//
// Note: the exception pc MUST be at a call (precise debug information)
//       Registers x10, x13, x12, x14, x15, t0 are not callee saved.
//

void OptoRuntime::generate_exception_blob() {
  assert(!OptoRuntime::is_callee_saved_register(R13_num), "");
  assert(!OptoRuntime::is_callee_saved_register(R10_num), "");
  assert(!OptoRuntime::is_callee_saved_register(R12_num), "");

  assert(SimpleRuntimeFrame::framesize % 4 == 0, "sp not 16-byte aligned");

  // Allocate space for the code
  ResourceMark rm;
  // Setup code generation tools
  CodeBuffer buffer("exception_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);
  assert_cond(masm != NULL);

  // TODO check various assumptions made here
  //
  // make sure we do so before running this

  address start = __ pc();

  // push fp and retaddr by hand
  // Exception pc is 'return address' for stack walker
  __ addi(sp, sp, -2 * wordSize);
  __ sd(lr, Address(sp, wordSize));
  __ sd(fp, Address(sp));
  // there are no callee save registers and we don't expect an
  // arg reg save area
#ifndef PRODUCT
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
  // Store exception in Thread object. We cannot pass any arguments to the
  // handle_exception call, since we do not want to make any assumption
  // about the size of the frame where the exception happened in.
  __ sd(x10, Address(xthread, JavaThread::exception_oop_offset()));
  __ sd(x13, Address(xthread, JavaThread::exception_pc_offset()));

  // This call does all the hard work.  It checks if an exception handler
  // exists in the method.
  // If so, it returns the handler address.
  // If not, it prepares for stack-unwinding, restoring the callee-save
  // registers of the frame being removed.
  //
  // address OptoRuntime::handle_exception_C(JavaThread* thread)
  //
  // n.b. 1 gp arg, 0 fp args, integral return type

  // the stack should always be aligned
  address the_pc = __ pc();
  __ set_last_Java_frame(sp, noreg, the_pc, t0);
  __ mv(c_rarg0, xthread);
  int32_t offset = 0;
  __ la_patchable(t0, RuntimeAddress(CAST_FROM_FN_PTR(address, OptoRuntime::handle_exception_C)), offset);
  __ jalr(x1, t0, offset);

  __ ifence();

  // Set an oopmap for the call site.  This oopmap will only be used if we
  // are unwinding the stack.  Hence, all locations will be dead.
  // Callee-saved registers will be the same as the frame above (i.e.,
  // handle_exception_stub), since they were restored when we got the
  // exception.

  OopMapSet* oop_maps = new OopMapSet();
  assert_cond(oop_maps != NULL);

  oop_maps->add_gc_map(the_pc - start, new OopMap(SimpleRuntimeFrame::framesize, 0));

  __ reset_last_Java_frame(false);

  // Restore callee-saved registers

  // fp is an implicitly saved callee saved register (i.e. the calling
  // convention will save restore it in prolog/epilog) Other than that
  // there are no callee save registers now that adapter frames are gone.
  // and we dont' expect an arg reg save area
  __ ld(fp, Address(sp));
  __ ld(x13, Address(sp, wordSize));
  __ addi(sp, sp , 2 * wordSize);

  // x10: exception handler

  // We have a handler in x10 (could be deopt blob).
  __ mv(t0, x10);

  // Get the exception oop
  __ ld(x10, Address(xthread, JavaThread::exception_oop_offset()));
  // Get the exception pc in case we are deoptimized
  __ ld(x14, Address(xthread, JavaThread::exception_pc_offset()));
#ifdef ASSERT
  __ sd(zr, Address(xthread, JavaThread::exception_handler_pc_offset()));
  __ sd(zr, Address(xthread, JavaThread::exception_pc_offset()));
#endif
  // Clear the exception oop so GC no longer processes it as a root.
  __ sd(zr, Address(xthread, JavaThread::exception_oop_offset()));

  // x10: exception oop
  // t0:  exception handler
  // x14: exception pc
  // Jump to handler

  __ jr(t0);

  // Make sure all code is generated
  masm->flush();

  // Set exception blob
  _exception_blob =  ExceptionBlob::create(&buffer, oop_maps, SimpleRuntimeFrame::framesize >> 1);
}
#endif // COMPILER2
