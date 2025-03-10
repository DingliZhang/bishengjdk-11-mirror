/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2018, Red Hat Inc. All rights reserved.
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

#ifndef CPU_RISCV64_VM_NATIVEINST_RISCV64_HPP
#define CPU_RISCV64_VM_NATIVEINST_RISCV64_HPP

#include "asm/assembler.hpp"
#include "runtime/icache.hpp"
#include "runtime/os.hpp"

// We have interfaces for the following instructions:
// - NativeInstruction
// - - NativeCall
// - - NativeMovConstReg
// - - NativeMovRegMem
// - - NativeJump
// - - NativeGeneralJump
// - - NativeIllegalInstruction
// - - NativeCallTrampolineStub
// - - NativeMembar
// - - NativeFenceI

// The base class for different kinds of native instruction abstractions.
// Provides the primitive operations to manipulate code relative to this.

class NativeInstruction {
  friend class Relocation;
  friend bool is_NativeCallTrampolineStub_at(address);
 public:
  enum {
    instruction_size = 4
  };

  juint encoding() const {
    return uint_at(0);
  }

  bool is_jal()                             const { return is_jal_at(addr_at(0));         }
  bool is_movptr()                          const { return is_movptr_at(addr_at(0));      }
  bool is_call()                            const { return is_call_at(addr_at(0));        }
  bool is_jump()                            const { return is_jump_at(addr_at(0));        }

  static bool is_jal_at(address instr)        { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b1101111; }
  static bool is_jalr_at(address instr)       { assert_cond(instr != NULL); return (Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b1100111 &&
                                                Assembler::extract(((unsigned*)instr)[0], 14, 12) == 0b000); }
  static bool is_branch_at(address instr)     { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b1100011; }
  static bool is_ld_at(address instr)         { assert_cond(instr != NULL); return (Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0000011 &&
                                                Assembler::extract(((unsigned*)instr)[0], 14, 12) == 0b011); }
  static bool is_load_at(address instr)       { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0000011; }
  static bool is_float_load_at(address instr) { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0000111; }
  static bool is_auipc_at(address instr)      { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0010111; }
  static bool is_jump_at(address instr)       { assert_cond(instr != NULL); return (is_branch_at(instr) || is_jal_at(instr) || is_jalr_at(instr)); }
  static bool is_addi_at(address instr)       { assert_cond(instr != NULL); return (Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0010011 &&
                                                Assembler::extract(((unsigned*)instr)[0], 14, 12) == 0b000); }
  static bool is_addiw_at(address instr)      { assert_cond(instr != NULL); return (Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0011011 &&
                                                Assembler::extract(((unsigned*)instr)[0], 14, 12) == 0b000); }
  static bool is_lui_at(address instr)        { assert_cond(instr != NULL); return Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0110111; }
  static bool is_slli_shift_at(address instr, uint32_t shift) {
    assert_cond(instr != NULL);
    return (Assembler::extract(((unsigned*)instr)[0], 6, 0) == 0b0010011 && // opcode field
            Assembler::extract(((unsigned*)instr)[0], 14, 12) == 0b001 &&   // funct3 field, select the type of operation
            Assembler::extract(((unsigned*)instr)[0], 25, 20) == shift);    // shamt field
  }

  // return true if the (index1~index2) field of instr1 is equal to (index3~index4) field of instr2, otherwise false
  static bool compare_instr_field(address instr1, int index1, int index2, address instr2, int index3, int index4) {
    assert_cond(instr1 != NULL && instr2 != NULL);
    return Assembler::extract(((unsigned*)instr1)[0], index1, index2) == Assembler::extract(((unsigned*)instr2)[0], index3, index4);
  }

  // the instruction sequence of movptr is as below:
  //     lui
  //     addi
  //     slli
  //     addi
  //     slli
  //     addi/jalr/load
  static bool check_movptr_data_dependency(address instr) {
    return compare_instr_field(instr + 4, 19, 15, instr, 11, 7)       &&     // check the rs1 field of addi and the rd field of lui
           compare_instr_field(instr + 4, 19, 15, instr + 4, 11, 7)   &&     // check the rs1 field and the rd field of addi
           compare_instr_field(instr + 8, 19, 15, instr + 4, 11, 7)   &&     // check the rs1 field of slli and the rd field of addi
           compare_instr_field(instr + 8, 19, 15, instr + 8, 11, 7)   &&     // check the rs1 field and the rd field of slli
           compare_instr_field(instr + 12, 19, 15, instr + 8, 11, 7)  &&     // check the rs1 field of addi and the rd field of slli
           compare_instr_field(instr + 12, 19, 15, instr + 12, 11, 7) &&     // check the rs1 field and the rd field of addi
           compare_instr_field(instr + 16, 19, 15, instr + 12, 11, 7) &&     // check the rs1 field of slli and the rd field of addi
           compare_instr_field(instr + 16, 19, 15, instr + 16, 11, 7) &&     // check the rs1 field and the rd field of slli
           compare_instr_field(instr + 20, 19, 15, instr + 16, 11, 7);       // check the rs1 field of addi/jalr/load and the rd field of slli
  }

  // the instruction sequence of li64 is as below:
  //     lui
  //     addi
  //     slli
  //     addi
  //     slli
  //     addi
  //     slli
  //     addi
  static bool check_li64_data_dependency(address instr) {
    return compare_instr_field(instr + 4, 19, 15, instr, 11, 7)       &&  // check the rs1 field of addi and the rd field of lui
           compare_instr_field(instr + 4, 19, 15, instr + 4, 11, 7)   &&  // check the rs1 field and the rd field of addi
           compare_instr_field(instr + 8, 19, 15, instr + 4, 11, 7)   &&  // check the rs1 field of slli and the rd field of addi
           compare_instr_field(instr + 8, 19, 15, instr + 8, 11, 7)   &&  // check the rs1 field and the rd field of slli
           compare_instr_field(instr + 12, 19, 15, instr + 8, 11, 7)  &&  // check the rs1 field of addi and the rd field of slli
           compare_instr_field(instr + 12, 19, 15, instr + 12, 11, 7) &&  // check the rs1 field and the rd field of addi
           compare_instr_field(instr + 16, 19, 15, instr + 12, 11, 7) &&  // check the rs1 field of slli and the rd field of addi
           compare_instr_field(instr + 16, 19, 15, instr + 16, 11, 7) &&  // check the rs1 field and the rd field fof slli
           compare_instr_field(instr + 20, 19, 15, instr + 16, 11, 7) &&  // check the rs1 field of addi and the rd field of slli
           compare_instr_field(instr + 20, 19, 15, instr + 20, 11, 7) &&  // check the rs1 field and the rd field of addi
           compare_instr_field(instr + 24, 19, 15, instr + 20, 11, 7) &&  // check the rs1 field of slli and the rd field of addi
           compare_instr_field(instr + 24, 19, 15, instr + 24, 11, 7) &&  // check the rs1 field and the rd field of slli
           compare_instr_field(instr + 28, 19, 15, instr + 24, 11, 7) &&  // check the rs1 field of addi and the rd field of slli
           compare_instr_field(instr + 28, 19, 15, instr + 28, 11, 7);    // check the rs1 field and the rd field of addi
  }

  // the instruction sequence of li32 is as below:
  //     lui
  //     addiw
  static bool check_li32_data_dependency(address instr) {
    return compare_instr_field(instr + 4, 19, 15, instr, 11, 7) &&     // check the rs1 field of addiw and the rd field of lui
           compare_instr_field(instr + 4, 19, 15, instr + 4, 11, 7);   // check the rs1 field and the rd field of addiw
  }

  // the instruction sequence of pc-relative is as below:
  //     auipc
  //     jalr/addi/load/float_load
  static bool check_pc_relative_data_dependency(address instr) {
    return compare_instr_field(instr, 11, 7, instr + 4, 19, 15);          // check the rd field of auipc and the rs1 field of jalr/addi/load/float_load
  }

  // the instruction sequence of load_label is as below:
  //     auipc
  //     load
  static bool check_load_pc_relative_data_dependency(address instr) {
    return compare_instr_field(instr, 11, 7, instr + 4, 11, 7) &&      // check the rd field of auipc and the rd field of load
           compare_instr_field(instr + 4, 19, 15, instr + 4, 11, 7);   // check the rs1 field of load and the rd field of load
  }

  static bool is_movptr_at(address instr);
  static bool is_li32_at(address instr);
  static bool is_li64_at(address instr);
  static bool is_pc_relative_at(address branch);
  static bool is_load_pc_relative_at(address branch);

  static bool is_call_at(address instr) {
    if (is_jal_at(instr) || is_jalr_at(instr)) {
      return true;
    }
    return false;
  }
  static bool is_lwu_to_zr(address instr);

  inline bool is_nop();
  inline bool is_illegal();
  inline bool is_return();
  inline bool is_jump_or_nop();
  inline bool is_cond_jump();
  bool is_safepoint_poll();
  bool is_sigill_zombie_not_entrant();

 protected:
  address addr_at(int offset) const    { return address(this) + offset; }

  jint int_at(int offset) const        { return *(jint*) addr_at(offset); }
  juint uint_at(int offset) const      { return *(juint*) addr_at(offset); }

  address ptr_at(int offset) const     { return *(address*) addr_at(offset); }

  oop  oop_at (int offset) const       { return *(oop*) addr_at(offset); }


  void set_int_at(int offset, jint  i)        { *(jint*)addr_at(offset) = i; }
  void set_uint_at(int offset, jint  i)       { *(juint*)addr_at(offset) = i; }
  void set_ptr_at (int offset, address  ptr)  { *(address*) addr_at(offset) = ptr; }
  void set_oop_at (int offset, oop  o)        { *(oop*) addr_at(offset) = o; }

 public:

  inline friend NativeInstruction* nativeInstruction_at(address addr);

  static bool maybe_cpool_ref(address instr) {
    return is_auipc_at(instr);
  }

  bool is_membar() {
    unsigned int insn = uint_at(0);
    return (insn & 0x7f) == 0b1111 && Assembler::extract(insn, 14, 12) == 0;
  }
};

inline NativeInstruction* nativeInstruction_at(address addr) {
  return (NativeInstruction*)addr;
}

// The natural type of an RISCV64 instruction is uint32_t
inline NativeInstruction* nativeInstruction_at(uint32_t *addr) {
  return (NativeInstruction*)addr;
}

inline NativeCall* nativeCall_at(address addr);
// The NativeCall is an abstraction for accessing/manipulating native
// call instructions (used to manipulate inline caches, primitive &
// DSO calls, etc.).

class NativeCall: public NativeInstruction {
 public:
  enum RISCV64_specific_constants {
    instruction_size            =    4,
    instruction_offset          =    0,
    displacement_offset         =    0,
    return_address_offset       =    4
  };

  address instruction_address() const       { return addr_at(instruction_offset); }
  address next_instruction_address() const  { return addr_at(return_address_offset); }
  address return_address() const            { return addr_at(return_address_offset); }
  address destination() const;

  void set_destination(address dest)      {
    if (is_jal()) {
      intptr_t offset = (intptr_t)(dest - instruction_address());
      assert((offset & 0x1) == 0, "should be aligned");
      assert(is_imm_in_range(offset, 20, 1), "set_destination, offset is too large to be patched in one jal insrusction\n");
      unsigned int insn = 0b1101111; // jal
      address pInsn = (address)(&insn);
      Assembler::patch(pInsn, 31, 31, (offset >> 20) & 0x1);
      Assembler::patch(pInsn, 30, 21, (offset >> 1) & 0x3ff);
      Assembler::patch(pInsn, 20, 20, (offset >> 11) & 0x1);
      Assembler::patch(pInsn, 19, 12, (offset >> 12) & 0xff);
      Assembler::patch(pInsn, 11, 7, lr->encoding()); // Rd must be x1, need lr
      set_int_at(displacement_offset, insn);
      return;
    }
    ShouldNotReachHere();
  }

  void  verify_alignment()                       { ; }
  void  verify();
  void  print();

  // Creation
  inline friend NativeCall* nativeCall_at(address addr);
  inline friend NativeCall* nativeCall_before(address return_address);

  static bool is_call_before(address return_address) {
    return is_call_at(return_address - NativeCall::return_address_offset);
  }

  // MT-safe patching of a call instruction.
  static void insert(address code_pos, address entry);

  static void replace_mt_safe(address instr_addr, address code_buffer);

  // Similar to replace_mt_safe, but just changes the destination.  The
  // important thing is that free-running threads are able to execute
  // this call instruction at all times.  If the call is an immediate BL
  // instruction we can simply rely on atomicity of 32-bit writes to
  // make sure other threads will see no intermediate states.

  // We cannot rely on locks here, since the free-running threads must run at
  // full speed.
  //
  // Used in the runtime linkage of calls; see class CompiledIC.
  // (Cf. 4506997 and 4479829, where threads witnessed garbage displacements.)

  // The parameter assert_lock disables the assertion during code generation.
  void set_destination_mt_safe(address dest, bool assert_lock = true);

  address get_trampoline();
};

inline NativeCall* nativeCall_at(address addr) {
  assert_cond(addr != NULL);
  NativeCall* call = (NativeCall*)(addr - NativeCall::instruction_offset);
#ifdef ASSERT
  call->verify();
#endif
  return call;
}

inline NativeCall* nativeCall_before(address return_address) {
  assert_cond(return_address != NULL);
  NativeCall* call = (NativeCall*)(return_address - NativeCall::return_address_offset);
#ifdef ASSERT
  call->verify();
#endif
  return call;
}

// An interface for accessing/manipulating native mov reg, imm instructions.
// (used to manipulate inlined 64-bit data calls, etc.)
class NativeMovConstReg: public NativeInstruction {
 public:
  enum RISCV64_specific_constants {
    movptr_instruction_size             =    6 * NativeInstruction::instruction_size, // lui, addi, slli, addi, slli, addi.  See movptr().
    movptr_with_offset_instruction_size =    5 * NativeInstruction::instruction_size, // lui, addi, slli, addi, slli. See movptr_with_offset().
    load_pc_relative_instruction_size   =    2 * NativeInstruction::instruction_size, // auipc, ld
    instruction_offset                  =    0,
    displacement_offset                 =    0
  };

  address instruction_address() const       { return addr_at(instruction_offset); }
  address next_instruction_address() const  {
    // if the instruction at 5 * instruction_size is addi,
    // it means a lui + addi + slli + addi + slli + addi instruction sequence,
    // and the next instruction address should be addr_at(6 * instruction_size).
    // However, when the instruction at 5 * instruction_size isn't addi,
    // the next instruction address should be addr_at(5 * instruction_size)
    if (nativeInstruction_at(instruction_address())->is_movptr()) {
      if (is_addi_at(addr_at(movptr_with_offset_instruction_size))) {
        // Assume: lui, addi, slli, addi, slli, addi
        return addr_at(movptr_instruction_size);
      } else {
        // Assume: lui, addi, slli, addi, slli
        return addr_at(movptr_with_offset_instruction_size);
      }
    } else if (is_load_pc_relative_at(instruction_address())) {
      // Assume: auipc, ld
      return addr_at(load_pc_relative_instruction_size);
    }
    guarantee(false, "Unknown instruction in NativeMovConstReg");
    return NULL;
  }

  intptr_t data() const;
  void  set_data(intptr_t x);

  void flush() {
    if (!maybe_cpool_ref(instruction_address())) {
      ICache::invalidate_range(instruction_address(), movptr_instruction_size);
    }
  }

  void  verify();
  void  print();

  // Creation
  inline friend NativeMovConstReg* nativeMovConstReg_at(address addr);
  inline friend NativeMovConstReg* nativeMovConstReg_before(address addr);
};

inline NativeMovConstReg* nativeMovConstReg_at(address addr) {
  assert_cond(addr != NULL);
  NativeMovConstReg* test = (NativeMovConstReg*)(addr - NativeMovConstReg::instruction_offset);
#ifdef ASSERT
  test->verify();
#endif
  return test;
}

inline NativeMovConstReg* nativeMovConstReg_before(address addr) {
  assert_cond(addr != NULL);
  NativeMovConstReg* test = (NativeMovConstReg*)(addr - NativeMovConstReg::instruction_size - NativeMovConstReg::instruction_offset);
#ifdef ASSERT
  test->verify();
#endif
  return test;
}

// RISCV64 should not use C1 runtime patching, so just leave NativeMovRegMem Unimplemented.
class NativeMovRegMem: public NativeInstruction {
 public:
  int instruction_start() const {
    Unimplemented();
    return 0;
  }

  address instruction_address() const {
    Unimplemented();
    return NULL;
  }

  int num_bytes_to_end_of_patch() const {
    Unimplemented();
    return 0;
  }

  int offset() const;

  void set_offset(int x);

  void add_offset_in_bytes(int add_offset) { Unimplemented(); }

  void verify();
  void print();

 private:
  inline friend NativeMovRegMem* nativeMovRegMem_at (address addr);
};

inline NativeMovRegMem* nativeMovRegMem_at (address addr) {
  Unimplemented();
  return NULL;
}

class NativeJump: public NativeInstruction {
 public:
  enum RISCV64_specific_constants {
    instruction_size            =    4,
    instruction_offset          =    0,
    data_offset                 =    0,
    next_instruction_offset     =    4
  };

  address instruction_address() const       { return addr_at(instruction_offset); }
  address next_instruction_address() const  { return addr_at(instruction_size); }
  address jump_destination() const;

  // Creation
  inline friend NativeJump* nativeJump_at(address address);

  void verify();

  // Unit testing stuff
  static void test() {}

  // Insertion of native jump instruction
  static void insert(address code_pos, address entry);
  // MT-safe insertion of native jump at verified method entry
  static void check_verified_entry_alignment(address entry, address verified_entry);
  static void patch_verified_entry(address entry, address verified_entry, address dest);
};

inline NativeJump* nativeJump_at(address addr) {
  NativeJump* jump = (NativeJump*)(addr - NativeJump::instruction_offset);
#ifdef ASSERT
  jump->verify();
#endif
  return jump;
}

class NativeGeneralJump: public NativeJump {
public:
  enum RISCV64_specific_constants {
    instruction_size            =    6 * NativeInstruction::instruction_size, // lui, addi, slli, addi, slli, jalr
    instruction_offset          =    0,
    data_offset                 =    0,
    next_instruction_offset     =    6 * NativeInstruction::instruction_size  // lui, addi, slli, addi, slli, jalr
  };

  address jump_destination() const;

  static void insert_unconditional(address code_pos, address entry);
  static void replace_mt_safe(address instr_addr, address code_buffer);
};

inline NativeGeneralJump* nativeGeneralJump_at(address addr) {
  assert_cond(addr != NULL);
  NativeGeneralJump* jump = (NativeGeneralJump*)(addr);
  debug_only(jump->verify();)
  return jump;
}

class NativeIllegalInstruction: public NativeInstruction {
 public:
  // Insert illegal opcode as specific address
  static void insert(address code_pos);
};

inline bool NativeInstruction::is_nop()         {
  uint32_t insn = *(uint32_t*)addr_at(0);
  return insn == 0x13;
}

inline bool NativeInstruction::is_jump_or_nop() {
  return is_nop() || is_jump();
}

// Call trampoline stubs.
class NativeCallTrampolineStub : public NativeInstruction {
 public:

  enum RISCV64_specific_constants {
    // Refer to function emit_trampoline_stub.
    instruction_size = 3 * NativeInstruction::instruction_size + wordSize, // auipc + ld + jr + target address
    data_offset      = 3 * NativeInstruction::instruction_size,            // auipc + ld + jr
  };

  address destination(nmethod *nm = NULL) const;
  void set_destination(address new_destination);
  ptrdiff_t destination_offset() const;
};

inline bool is_NativeCallTrampolineStub_at(address addr) {
  // Ensure that the stub is exactly
  //      ld   t0, L--->auipc + ld
  //      jr   t0
  // L:

  // judge inst + register + imm
  // 1). check the instructions: auipc + ld + jalr
  // 2). check if auipc[11:7] == t0 and ld[11:7] == t0 and ld[19:15] == t0 && jr[19:15] == t0
  // 3). check if the offset in ld[31:20] equals the data_offset
  assert_cond(addr != NULL);
  if (NativeInstruction::is_auipc_at(addr) && NativeInstruction::is_ld_at(addr + 4) && NativeInstruction::is_jalr_at(addr + 8) &&
      ((Register)(intptr_t)Assembler::extract(((unsigned*)addr)[0], 11, 7)     == x5) &&
      ((Register)(intptr_t)Assembler::extract(((unsigned*)addr)[1], 11, 7)     == x5) &&
      ((Register)(intptr_t)Assembler::extract(((unsigned*)addr)[1], 19, 15)    == x5) &&
      ((Register)(intptr_t)Assembler::extract(((unsigned*)addr)[2], 19, 15)    == x5) &&
      (Assembler::extract(((unsigned*)addr)[1], 31, 20) == NativeCallTrampolineStub::data_offset)) {
    return true;
  }
  return false;
}

inline NativeCallTrampolineStub* nativeCallTrampolineStub_at(address addr) {
  assert_cond(addr != NULL);
  assert(is_NativeCallTrampolineStub_at(addr), "no call trampoline found");
  return (NativeCallTrampolineStub*)addr;
}

class NativeMembar : public NativeInstruction {
public:
  uint32_t get_kind();
  void set_kind(uint32_t order_kind);
};

inline NativeMembar *NativeMembar_at(address addr) {
  assert_cond(addr != NULL);
  assert(nativeInstruction_at(addr)->is_membar(), "no membar found");
  return (NativeMembar*)addr;
}

class NativeFenceI : public NativeInstruction {
 public:
  static int instruction_size() {
    return (UseConservativeFence ? 2 : 1) * NativeInstruction::instruction_size;
  }
};

#endif // CPU_RISCV64_VM_NATIVEINST_RISCV64_HPP
