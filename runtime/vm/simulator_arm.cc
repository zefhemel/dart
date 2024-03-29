// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include <math.h>  // for isnan.
#include <setjmp.h>
#include <stdlib.h>

#include "vm/globals.h"
#if defined(TARGET_ARCH_ARM)

// Only build the simulator if not compiling for real ARM hardware.
#if !defined(HOST_ARCH_ARM)

#include "vm/simulator.h"

#include "vm/assembler.h"
#include "vm/constants_arm.h"
#include "vm/disassembler.h"
#include "vm/native_arguments.h"
#include "vm/thread.h"

namespace dart {

DEFINE_FLAG(bool, trace_sim, false, "Trace simulator execution.");
DEFINE_FLAG(int, stop_sim_at, 0, "Address to stop simulator at.");


// This macro provides a platform independent use of sscanf. The reason for
// SScanF not being implemented in a platform independent way through
// OS in the same way as SNPrint is that the Windows C Run-Time
// Library does not provide vsscanf.
#define SScanF sscanf  // NOLINT


// Unimplemented counter class for debugging and measurement purposes.
class StatsCounter {
 public:
  explicit StatsCounter(const char* name) {
    // UNIMPLEMENTED();
  }

  void Increment() {
    // UNIMPLEMENTED();
  }
};


// SimulatorSetjmpBuffer are linked together, and the last created one
// is referenced by the Simulator. When an exception is thrown, the exception
// runtime looks at where to jump and finds the corresponding
// SimulatorSetjmpBuffer based on the stack pointer of the exception handler.
// The runtime then does a Longjmp on that buffer to return to the simulator.
class SimulatorSetjmpBuffer {
 public:
  int Setjmp() { return setjmp(buffer_); }
  void Longjmp() {
    // "This" is now the last setjmp buffer.
    simulator_->set_last_setjmp_buffer(this);
    longjmp(buffer_, 1);
  }

  explicit SimulatorSetjmpBuffer(Simulator* sim) {
    simulator_ = sim;
    link_ = sim->last_setjmp_buffer();
    sim->set_last_setjmp_buffer(this);
    sp_ = sim->get_register(SP);
  }

  ~SimulatorSetjmpBuffer() {
    ASSERT(simulator_->last_setjmp_buffer() == this);
    simulator_->set_last_setjmp_buffer(link_);
  }

  SimulatorSetjmpBuffer* link() { return link_; }

  int32_t sp() { return sp_; }

 private:
  int32_t sp_;
  Simulator* simulator_;
  SimulatorSetjmpBuffer* link_;
  jmp_buf buffer_;

  friend class Simulator;
};


// The SimulatorDebugger class is used by the simulator while debugging
// simulated ARM code.
class SimulatorDebugger {
 public:
  explicit SimulatorDebugger(Simulator* sim);
  ~SimulatorDebugger();

  void Stop(Instr* instr, const char* message);
  void Debug();

  char* ReadLine(const char* prompt);

 private:
  static const int32_t kSimulatorBreakpointInstr =  // svc #kBreakpointSvcCode
    ((AL << kConditionShift) | (0xf << 24) | kBreakpointSvcCode);
  static const int32_t kNopInstr =  // nop
    ((AL << kConditionShift) | (0x32 << 20) | (0xf << 12));

  Simulator* sim_;

  bool GetValue(char* desc, uint32_t* value);
  bool GetFValue(char* desc, float* value);
  bool GetDValue(char* desc, double* value);

  // Set or delete a breakpoint. Returns true if successful.
  bool SetBreakpoint(Instr* breakpc);
  bool DeleteBreakpoint(Instr* breakpc);

  // Undo and redo all breakpoints. This is needed to bracket disassembly and
  // execution to skip past breakpoints when run from the debugger.
  void UndoBreakpoints();
  void RedoBreakpoints();
};


SimulatorDebugger::SimulatorDebugger(Simulator* sim) {
  sim_ = sim;
}


SimulatorDebugger::~SimulatorDebugger() {
}


void SimulatorDebugger::Stop(Instr* instr, const char* message) {
  OS::Print("Simulator hit %s\n", message);
  Debug();
}


static Register LookupCpuRegisterByName(const char* name) {
  static const char* kNames[] = {
      "r0",  "r1",  "r2",  "r3",
      "r4",  "r5",  "r6",  "r7",
      "r8",  "r9",  "r10", "r11",
      "r12", "r13", "r14", "r15",
      "pc",  "lr",  "sp",  "ip",
      "fp",  "pp",  "ctx"
  };
  static const Register kRegisters[] = {
      R0,  R1,  R2,  R3,
      R4,  R5,  R6,  R7,
      R8,  R9,  R10, R11,
      R12, R13, R14, R15,
      PC,  LR,  SP,  IP,
      FP,  R10, R9
  };
  ASSERT(ARRAY_SIZE(kNames) == ARRAY_SIZE(kRegisters));
  for (unsigned i = 0; i < ARRAY_SIZE(kNames); i++) {
    if (strcmp(kNames[i], name) == 0) {
      return kRegisters[i];
    }
  }
  return kNoRegister;
}


static SRegister LookupSRegisterByName(const char* name) {
  int reg_nr = -1;
  bool ok = SScanF(name, "s%d", &reg_nr);
  if (ok && (0 <= reg_nr) && (reg_nr < kNumberOfSRegisters)) {
    return static_cast<SRegister>(reg_nr);
  }
  return kNoSRegister;
}


static DRegister LookupDRegisterByName(const char* name) {
  int reg_nr = -1;
  bool ok = SScanF(name, "d%d", &reg_nr);
  if (ok && (0 <= reg_nr) && (reg_nr < kNumberOfDRegisters)) {
    return static_cast<DRegister>(reg_nr);
  }
  return kNoDRegister;
}


bool SimulatorDebugger::GetValue(char* desc, uint32_t* value) {
  Register reg = LookupCpuRegisterByName(desc);
  if (reg != kNoRegister) {
    if (reg == PC) {
      *value = sim_->get_pc();
    } else {
      *value = sim_->get_register(reg);
    }
    return true;
  }
  if ((desc[0] == '*')) {
    uint32_t addr;
    if (GetValue(desc + 1, &addr)) {
      if (Simulator::IsIllegalAddress(addr)) {
        return false;
      }
      *value = *(reinterpret_cast<uint32_t*>(addr));
      return true;
    }
  }
  bool retval = SScanF(desc, "0x%x", value) == 1;
  if (!retval) {
    retval = SScanF(desc, "%x", value) == 1;
  }
  return retval;
}


bool SimulatorDebugger::GetFValue(char* desc, float* value) {
  SRegister sreg = LookupSRegisterByName(desc);
  if (sreg != kNoSRegister) {
    *value = sim_->get_sregister(sreg);
    return true;
  }
  if ((desc[0] == '*')) {
    uint32_t addr;
    if (GetValue(desc + 1, &addr)) {
      if (Simulator::IsIllegalAddress(addr)) {
        return false;
      }
      *value = *(reinterpret_cast<float*>(addr));
      return true;
    }
  }
  return false;
}


bool SimulatorDebugger::GetDValue(char* desc, double* value) {
  DRegister dreg = LookupDRegisterByName(desc);
  if (dreg != kNoDRegister) {
    *value = sim_->get_dregister(dreg);
    return true;
  }
  if ((desc[0] == '*')) {
    uint32_t addr;
    if (GetValue(desc + 1, &addr)) {
      if (Simulator::IsIllegalAddress(addr)) {
        return false;
      }
      *value = *(reinterpret_cast<double*>(addr));
      return true;
    }
  }
  return false;
}


bool SimulatorDebugger::SetBreakpoint(Instr* breakpc) {
  // Check if a breakpoint can be set. If not return without any side-effects.
  if (sim_->break_pc_ != NULL) {
    return false;
  }

  // Set the breakpoint.
  sim_->break_pc_ = breakpc;
  sim_->break_instr_ = breakpc->InstructionBits();
  // Not setting the breakpoint instruction in the code itself. It will be set
  // when the debugger shell continues.
  return true;
}


bool SimulatorDebugger::DeleteBreakpoint(Instr* breakpc) {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }

  sim_->break_pc_ = NULL;
  sim_->break_instr_ = 0;
  return true;
}


void SimulatorDebugger::UndoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }
}


void SimulatorDebugger::RedoBreakpoints() {
  if (sim_->break_pc_ != NULL) {
    sim_->break_pc_->SetInstructionBits(kSimulatorBreakpointInstr);
  }
}


void SimulatorDebugger::Debug() {
  intptr_t last_pc = -1;
  bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];

  // make sure to have a proper terminating character if reaching the limit
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  // Undo all set breakpoints while running in the debugger shell. This will
  // make them invisible to all commands.
  UndoBreakpoints();

  while (!done) {
    if (last_pc != sim_->get_pc()) {
      last_pc = sim_->get_pc();
      Disassembler::Disassemble(last_pc, last_pc + Instr::kInstrSize);
    }
    char* line = ReadLine("sim> ");
    if (line == NULL) {
      FATAL("ReadLine failed");
    } else {
      // Use sscanf to parse the individual parts of the command line. At the
      // moment no command expects more than two parameters.
      int args = SScanF(line,
                        "%" XSTR(COMMAND_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s "
                        "%" XSTR(ARG_SIZE) "s",
                        cmd, arg1, arg2);
      if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        OS::Print("c/cont -- continue execution\n"
                  "disasm -- disassemble instrs at current pc location\n"
                  "  other variants are:\n"
                  "    disasm <address>\n"
                  "    disasm <address> <number_of_instructions>\n"
                  "  by default 10 instrs are disassembled\n"
                  "del -- delete breakpoints\n"
                  "flags -- print flag values\n"
                  "gdb -- transfer control to gdb\n"
                  "h/help -- print this help string\n"
                  "break <address> -- set break point at specified address\n"
                  "p/print <reg or value or *addr> -- print integer value\n"
                  "pf/printfloat <sreg or *addr> -- print float value\n"
                  "pd/printdouble <dreg or *addr> -- print double value\n"
                  "po/printobject <*reg or *addr> -- print object\n"
                  "si/stepi -- single step an instruction\n"
                  "unstop -- if current pc is a stop instr make it a nop\n"
                  "q/quit -- Quit the debugger and exit the program\n");
      } else if ((strcmp(cmd, "quit") == 0) || (strcmp(cmd, "q") == 0)) {
        OS::Print("Quitting\n");
        OS::Exit(0);
      } else if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        sim_->InstructionDecode(reinterpret_cast<Instr*>(sim_->get_pc()));
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // Execute the one instruction we broke at with breakpoints disabled.
        sim_->InstructionDecode(reinterpret_cast<Instr*>(sim_->get_pc()));
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (args == 2) {
          uint32_t value;
          if (GetValue(arg1, &value)) {
            OS::Print("%s: %u 0x%x\n", arg1, value, value);
          } else {
            OS::Print("%s unrecognized\n", arg1);
          }
        } else {
          OS::Print("print <reg or value or *addr>\n");
        }
      } else if ((strcmp(cmd, "pf") == 0) || (strcmp(cmd, "printfloat") == 0)) {
        if (args == 2) {
          float fvalue;
          if (GetFValue(arg1, &fvalue)) {
            uint32_t value = bit_cast<uint32_t, float>(fvalue);
            OS::Print("%s: 0%u 0x%x %.8g\n", arg1, value, value, fvalue);
          } else {
            OS::Print("%s unrecognized\n", arg1);
          }
        } else {
          OS::Print("printfloat <sreg or *addr>\n");
        }
      } else if ((strcmp(cmd, "pd") == 0) ||
                 (strcmp(cmd, "printdouble") == 0)) {
        if (args == 2) {
          double dvalue;
          if (GetDValue(arg1, &dvalue)) {
            uint64_t long_value = bit_cast<uint64_t, double>(dvalue);
            OS::Print("%s: %llu 0x%llx %.8g\n",
                arg1, long_value, long_value, dvalue);
          } else {
            OS::Print("%s unrecognized\n", arg1);
          }
        } else {
          OS::Print("printdouble <dreg or *addr>\n");
        }
      } else if ((strcmp(cmd, "po") == 0) ||
                 (strcmp(cmd, "printobject") == 0)) {
        if (args == 2) {
          uint32_t value;
          // Make the dereferencing '*' optional.
          if (((arg1[0] == '*') && GetValue(arg1 + 1, &value)) ||
              GetValue(arg1, &value)) {
            if (Isolate::Current()->heap()->Contains(value)) {
              OS::Print("%s: \n", arg1);
#if defined(DEBUG)
              const Object& obj = Object::Handle(
                  reinterpret_cast<RawObject*>(value));
              obj.Print();
#endif  // defined(DEBUG)
            } else {
              OS::Print("0x%x is not an object reference\n", value);
            }
          } else {
            OS::Print("%s unrecognized\n", arg1);
          }
        } else {
          OS::Print("printobject <*reg or *addr>\n");
        }
      } else if (strcmp(cmd, "disasm") == 0) {
        uint32_t start = 0;
        uint32_t end = 0;
        if (args == 1) {
          start = sim_->get_pc();
          end = start + (10 * Instr::kInstrSize);
        } else if (args == 2) {
          if (GetValue(arg1, &start)) {
            // no length parameter passed, assume 10 instructions
            end = start + (10 * Instr::kInstrSize);
          }
        } else {
          uint32_t length;
          if (GetValue(arg1, &start) && GetValue(arg2, &length)) {
            end = start + (length * Instr::kInstrSize);
          }
        }

        Disassembler::Disassemble(start, end);
      } else if (strcmp(cmd, "gdb") == 0) {
        OS::Print("relinquishing control to gdb\n");
        OS::DebugBreak();
        OS::Print("regaining control from gdb\n");
      } else if (strcmp(cmd, "break") == 0) {
        if (args == 2) {
          uint32_t addr;
          if (GetValue(arg1, &addr)) {
            if (!SetBreakpoint(reinterpret_cast<Instr*>(addr))) {
              OS::Print("setting breakpoint failed\n");
            }
          } else {
            OS::Print("%s unrecognized\n", arg1);
          }
        } else {
          OS::Print("break <addr>\n");
        }
      } else if (strcmp(cmd, "del") == 0) {
        if (!DeleteBreakpoint(NULL)) {
          OS::Print("deleting breakpoint failed\n");
        }
      } else if (strcmp(cmd, "flags") == 0) {
        OS::Print("APSR: ");
        OS::Print("N flag: %d; ", sim_->n_flag_);
        OS::Print("Z flag: %d; ", sim_->z_flag_);
        OS::Print("C flag: %d; ", sim_->c_flag_);
        OS::Print("V flag: %d\n", sim_->v_flag_);
        OS::Print("FPSCR: ");
        OS::Print("N flag: %d; ", sim_->fp_n_flag_);
        OS::Print("Z flag: %d; ", sim_->fp_z_flag_);
        OS::Print("C flag: %d; ", sim_->fp_c_flag_);
        OS::Print("V flag: %d\n", sim_->fp_v_flag_);
      } else if (strcmp(cmd, "unstop") == 0) {
        intptr_t stop_pc = sim_->get_pc() - Instr::kInstrSize;
        Instr* stop_instr = reinterpret_cast<Instr*>(stop_pc);
        if (stop_instr->IsSvc() || stop_instr->IsBkpt()) {
          stop_instr->SetInstructionBits(kNopInstr);
        } else {
          OS::Print("Not at debugger stop.\n");
        }
      } else {
        OS::Print("Unknown command: %s\n", cmd);
      }
    }
    delete[] line;
  }

  // Add all the breakpoints back to stop execution and enter the debugger
  // shell when hit.
  RedoBreakpoints();

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}


char* SimulatorDebugger::ReadLine(const char* prompt) {
  char* result = NULL;
  char line_buf[256];
  int offset = 0;
  bool keep_going = true;
  fprintf(stdout, "%s", prompt);
  fflush(stdout);
  while (keep_going) {
    if (fgets(line_buf, sizeof(line_buf), stdin) == NULL) {
      // fgets got an error. Just give up.
      if (result != NULL) {
        delete[] result;
      }
      return NULL;
    }
    int len = strlen(line_buf);
    if (len > 1 &&
        line_buf[len - 2] == '\\' &&
        line_buf[len - 1] == '\n') {
      // When we read a line that ends with a "\" we remove the escape and
      // append the remainder.
      line_buf[len - 2] = '\n';
      line_buf[len - 1] = 0;
      len -= 1;
    } else if ((len > 0) && (line_buf[len - 1] == '\n')) {
      // Since we read a new line we are done reading the line. This
      // will exit the loop after copying this buffer into the result.
      keep_going = false;
    }
    if (result == NULL) {
      // Allocate the initial result and make room for the terminating '\0'
      result = new char[len + 1];
      if (result == NULL) {
        // OOM, so cannot readline anymore.
        return NULL;
      }
    } else {
      // Allocate a new result with enough room for the new addition.
      int new_len = offset + len + 1;
      char* new_result = new char[new_len];
      if (new_result == NULL) {
        // OOM, free the buffer allocated so far and return NULL.
        delete[] result;
        return NULL;
      } else {
        // Copy the existing input into the new array and set the new
        // array as the result.
        memmove(new_result, result, offset);
        delete[] result;
        result = new_result;
      }
    }
    // Copy the newly read line into the result.
    memmove(result + offset, line_buf, len);
    offset += len;
  }
  ASSERT(result != NULL);
  result[offset] = '\0';
  return result;
}


// Synchronization primitives support.
Mutex* Simulator::exclusive_access_lock_ = NULL;
Simulator::AddressTag Simulator::exclusive_access_state_[kNumAddressTags];
int Simulator::next_address_tag_;


void Simulator::SetExclusiveAccess(uword addr) {
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  int i = 0;
  while ((i < kNumAddressTags) &&
         (exclusive_access_state_[i].isolate != isolate)) {
    i++;
  }
  if (i == kNumAddressTags) {
    i = next_address_tag_;
    if (++next_address_tag_ == kNumAddressTags) next_address_tag_ = 0;
    exclusive_access_state_[i].isolate = isolate;
  }
  exclusive_access_state_[i].addr = addr;
}


bool Simulator::HasExclusiveAccessAndOpen(uword addr) {
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  bool result = false;
  for (int i = 0; i < kNumAddressTags; i++) {
    if (exclusive_access_state_[i].isolate == isolate) {
      if (exclusive_access_state_[i].addr == addr) {
        result = true;
      }
      exclusive_access_state_[i].addr = NULL;
      continue;
    }
    if (exclusive_access_state_[i].addr == addr) {
      exclusive_access_state_[i].addr = NULL;
    }
  }
  return result;
}


void Simulator::InitOnce() {
  // Setup exclusive access state.
  exclusive_access_lock_ = new Mutex();
  for (int i = 0; i < kNumAddressTags; i++) {
    exclusive_access_state_[i].isolate = NULL;
    exclusive_access_state_[i].addr = NULL;
  }
  next_address_tag_ = 0;
}


Simulator::Simulator() {
  // Setup simulator support first. Some of this information is needed to
  // setup the architecture state.
  // We allocate the stack here, the size is computed as the sum of
  // the size specified by the user and the buffer space needed for
  // handling stack overflow exceptions. To be safe in potential
  // stack underflows we also add some underflow buffer space.
  stack_ = new char[(Isolate::GetSpecifiedStackSize() +
                     Isolate::kStackSizeBuffer +
                     kSimulatorStackUnderflowSize)];
  pc_modified_ = false;
  icount_ = 0;
  break_pc_ = NULL;
  break_instr_ = 0;
  last_setjmp_buffer_ = NULL;
  top_exit_frame_info_ = 0;

  // Setup architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < kNumberOfCpuRegisters; i++) {
    registers_[i] = 0;
  }
  n_flag_ = false;
  z_flag_ = false;
  c_flag_ = false;
  v_flag_ = false;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area.
  registers_[SP] = StackTop();
  // The lr and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[PC] = kBadLR;
  registers_[LR] = kBadLR;

  // All double-precision registers are initialized to zero.
  for (int i = 0; i < kNumberOfDRegisters; i++) {
    dregisters_[i] = 0.0;
  }
  // Since VFP registers are overlapping, single-precision registers should
  // already be initialized.
  ASSERT(2*kNumberOfDRegisters >= kNumberOfSRegisters);
  for (int i = 0; i < kNumberOfSRegisters; i++) {
    ASSERT(sregisters_[i] == 0.0);
  }
  fp_n_flag_ = false;
  fp_z_flag_ = false;
  fp_c_flag_ = false;
  fp_v_flag_ = false;
}


Simulator::~Simulator() {
  delete[] stack_;
  Isolate* isolate = Isolate::Current();
  if (isolate != NULL) {
    isolate->set_simulator(NULL);
  }
}


// When the generated code calls an external reference we need to catch that in
// the simulator.  The external reference will be a function compiled for the
// host architecture.  We need to call that function instead of trying to
// execute it with the simulator.  We do that by redirecting the external
// reference to a svc (supervisor call) instruction that is handled by
// the simulator.  We write the original destination of the jump just at a known
// offset from the svc instruction so the simulator knows what to call.
class Redirection {
 public:
  uword address_of_svc_instruction() {
    return reinterpret_cast<uword>(&svc_instruction_);
  }

  uword external_function() const { return external_function_; }

  Simulator::CallKind call_kind() const { return call_kind_; }

  static Redirection* Get(uword external_function,
                          Simulator::CallKind call_kind) {
    Redirection* current;
    for (current = list_; current != NULL; current = current->next_) {
      if (current->external_function_ == external_function) return current;
    }
    return new Redirection(external_function, call_kind);
  }

  static Redirection* FromSvcInstruction(Instr* svc_instruction) {
    char* addr_of_svc = reinterpret_cast<char*>(svc_instruction);
    char* addr_of_redirection =
        addr_of_svc - OFFSET_OF(Redirection, svc_instruction_);
    return reinterpret_cast<Redirection*>(addr_of_redirection);
  }

 private:
  static const int32_t kRedirectSvcInstruction =
    ((AL << kConditionShift) | (0xf << 24) | kRedirectionSvcCode);
  Redirection(uword external_function, Simulator::CallKind call_kind)
      : external_function_(external_function),
        call_kind_(call_kind),
        svc_instruction_(kRedirectSvcInstruction),
        next_(list_) {
    list_ = this;
  }

  uword external_function_;
  Simulator::CallKind call_kind_;
  uint32_t svc_instruction_;
  Redirection* next_;
  static Redirection* list_;
};


Redirection* Redirection::list_ = NULL;


uword Simulator::RedirectExternalReference(uword function, CallKind call_kind) {
  Redirection* redirection = Redirection::Get(function, call_kind);
  return redirection->address_of_svc_instruction();
}


// Get the active Simulator for the current isolate.
Simulator* Simulator::Current() {
  Simulator* simulator = Isolate::Current()->simulator();
  if (simulator == NULL) {
    simulator = new Simulator();
    Isolate::Current()->set_simulator(simulator);
  }
  return simulator;
}


// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void Simulator::set_register(Register reg, int32_t value) {
  ASSERT((reg >= 0) && (reg < kNumberOfCpuRegisters));
  if (reg == PC) {
    pc_modified_ = true;
  }
  registers_[reg] = value;
}


// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int32_t Simulator::get_register(Register reg) const {
  ASSERT((reg >= 0) && (reg < kNumberOfCpuRegisters));
  return registers_[reg] + ((reg == PC) ? Instr::kPCReadOffset : 0);
}


// Raw access to the PC register.
void Simulator::set_pc(int32_t value) {
  pc_modified_ = true;
  registers_[PC] = value;
}


// Raw access to the PC register without the special adjustment when reading.
int32_t Simulator::get_pc() const {
  return registers_[PC];
}


// Accessors for VFP register state.
void Simulator::set_sregister(SRegister reg, float value) {
  ASSERT((reg >= 0) && (reg < kNumberOfSRegisters));
  sregisters_[reg] = value;
}


float Simulator::get_sregister(SRegister reg) const {
  ASSERT((reg >= 0) && (reg < kNumberOfSRegisters));
  return sregisters_[reg];
}


void Simulator::set_dregister(DRegister reg, double value) {
  ASSERT((reg >= 0) && (reg < kNumberOfDRegisters));
  dregisters_[reg] = value;
}


double Simulator::get_dregister(DRegister reg) const {
  ASSERT((reg >= 0) && (reg < kNumberOfDRegisters));
  return dregisters_[reg];
}


void Simulator::HandleIllegalAccess(uword addr, Instr* instr) {
  uword fault_pc = get_pc();
  // The debugger will not be able to single step past this instruction, but
  // it will be possible to disassemble the code and inspect registers.
  char buffer[128];
  snprintf(buffer, sizeof(buffer),
           "illegal memory access at 0x%"Px", pc=0x%"Px"\n",
           addr, fault_pc);
  SimulatorDebugger dbg(this);
  dbg.Stop(instr, buffer);
  // The debugger will return control in non-interactive mode.
  FATAL("Cannot continue execution after illegal memory access.");
}


// Processor versions prior to ARMv7 could not do unaligned reads and writes.
// On some ARM platforms an interrupt is caused.  On others it does a funky
// rotation thing.  However, from version v7, unaligned access is supported.
// Note that simulator runs have the runtime system running directly on the host
// system and only generated code is executed in the simulator.  Since the host
// is typically IA32 we will get the correct ARMv7-like behaviour on unaligned
// accesses, but we should actually not generate code accessing unaligned data,
// so we still want to know and abort if we encounter such code.
void Simulator::UnalignedAccess(const char* msg, uword addr, Instr* instr) {
  // The debugger will not be able to single step past this instruction, but
  // it will be possible to disassemble the code and inspect registers.
  char buffer[64];
  snprintf(buffer, sizeof(buffer),
           "unaligned %s at 0x%"Px", pc=%p\n", msg, addr, instr);
  SimulatorDebugger dbg(this);
  dbg.Stop(instr, buffer);
  // The debugger will return control in non-interactive mode.
  FATAL("Cannot continue execution after unaligned access.");
}


void Simulator::UnimplementedInstruction(Instr* instr) {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "Unimplemented instruction: pc=%p\n", instr);
  SimulatorDebugger dbg(this);
  dbg.Stop(instr, buffer);
  FATAL("Cannot continue execution after unimplemented instruction.");
}


int Simulator::ReadW(uword addr, Instr* instr) {
  static StatsCounter counter_read_w("Simulated word reads");
  counter_read_w.Increment();
  if ((addr & 3) == 0) {
    intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
    return *ptr;
  }
  UnalignedAccess("read", addr, instr);
  return 0;
}


void Simulator::WriteW(uword addr, int value, Instr* instr) {
  static StatsCounter counter_write_w("Simulated word writes");
  counter_write_w.Increment();
  if ((addr & 3) == 0) {
    intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
    *ptr = value;
    return;
  }
  UnalignedAccess("write", addr, instr);
}


uint16_t Simulator::ReadHU(uword addr, Instr* instr) {
  static StatsCounter counter_read_hu("Simulated unsigned halfword reads");
  counter_read_hu.Increment();
  if ((addr & 1) == 0) {
    uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
    return *ptr;
  }
  UnalignedAccess("unsigned halfword read", addr, instr);
  return 0;
}


int16_t Simulator::ReadH(uword addr, Instr* instr) {
  static StatsCounter counter_read_h("Simulated signed halfword reads");
  counter_read_h.Increment();
  if ((addr & 1) == 0) {
    int16_t* ptr = reinterpret_cast<int16_t*>(addr);
    return *ptr;
  }
  UnalignedAccess("signed halfword read", addr, instr);
  return 0;
}


void Simulator::WriteH(uword addr, uint16_t value, Instr* instr) {
  static StatsCounter counter_write_h("Simulated halfword writes");
  counter_write_h.Increment();
  if ((addr & 1) == 0) {
    uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
    *ptr = value;
    return;
  }
  UnalignedAccess("halfword write", addr, instr);
}


uint8_t Simulator::ReadBU(uword addr) {
  static StatsCounter counter_read_bu("Simulated unsigned byte reads");
  counter_read_bu.Increment();
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  return *ptr;
}


int8_t Simulator::ReadB(uword addr) {
  static StatsCounter counter_read_b("Simulated signed byte reads");
  counter_read_b.Increment();
  int8_t* ptr = reinterpret_cast<int8_t*>(addr);
  return *ptr;
}


void Simulator::WriteB(uword addr, uint8_t value) {
  static StatsCounter counter_write_b("Simulated byte writes");
  counter_write_b.Increment();
  uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
  *ptr = value;
}


// Synchronization primitives support.
void Simulator::ClearExclusive() {
  // This lock is initialized in Simulator::InitOnce().
  MutexLocker ml(exclusive_access_lock_);
  // Set exclusive access to open state for this isolate.
  HasExclusiveAccessAndOpen(NULL);
}


int Simulator::ReadExclusiveW(uword addr, Instr* instr) {
  // This lock is initialized in Simulator::InitOnce().
  MutexLocker ml(exclusive_access_lock_);
  SetExclusiveAccess(addr);
  return ReadW(addr, instr);
}


int Simulator::WriteExclusiveW(uword addr, int value, Instr* instr) {
  // This lock is initialized in Simulator::InitOnce().
  MutexLocker ml(exclusive_access_lock_);
  bool write_allowed = HasExclusiveAccessAndOpen(addr);
  if (write_allowed) {
    WriteW(addr, value, instr);
    return 0;  // Success.
  }
  return 1;  // Failure.
}


uword Simulator::CompareExchange(uword* address,
                                 uword compare_value,
                                 uword new_value) {
  // This lock is initialized in Simulator::InitOnce().
  MutexLocker ml(exclusive_access_lock_);
  uword value = *address;
  if (value == compare_value) {
    *address = new_value;
    // Same effect on exclusive access state as a successful STREX.
    HasExclusiveAccessAndOpen(reinterpret_cast<uword>(address));
  } else {
    // Same effect on exclusive access state as an LDREX.
    SetExclusiveAccess(reinterpret_cast<uword>(address));
  }
  return value;
}


// Returns the top of the stack area to enable checking for stack pointer
// validity.
uword Simulator::StackTop() const {
  // To be safe in potential stack underflows we leave some buffer above and
  // set the stack top.
  return reinterpret_cast<uword>(stack_) +
      (Isolate::GetSpecifiedStackSize() + Isolate::kStackSizeBuffer);
}


// Unsupported instructions use Format to print an error and stop execution.
void Simulator::Format(Instr* instr, const char* format) {
  OS::Print("Simulator found unsupported instruction:\n 0x%p: %s\n",
            instr,
            format);
  UNIMPLEMENTED();
}


// Checks if the current instruction should be executed based on its
// condition bits.
bool Simulator::ConditionallyExecute(Instr* instr) {
  switch (instr->ConditionField()) {
    case EQ: return z_flag_;
    case NE: return !z_flag_;
    case CS: return c_flag_;
    case CC: return !c_flag_;
    case MI: return n_flag_;
    case PL: return !n_flag_;
    case VS: return v_flag_;
    case VC: return !v_flag_;
    case HI: return c_flag_ && !z_flag_;
    case LS: return !c_flag_ || z_flag_;
    case GE: return n_flag_ == v_flag_;
    case LT: return n_flag_ != v_flag_;
    case GT: return !z_flag_ && (n_flag_ == v_flag_);
    case LE: return z_flag_ || (n_flag_ != v_flag_);
    case AL: return true;
    default: UNREACHABLE();
  }
  return false;
}


// Calculate and set the Negative and Zero flags.
void Simulator::SetNZFlags(int32_t val) {
  n_flag_ = (val < 0);
  z_flag_ = (val == 0);
}


// Set the Carry flag.
void Simulator::SetCFlag(bool val) {
  c_flag_ = val;
}


// Set the oVerflow flag.
void Simulator::SetVFlag(bool val) {
  v_flag_ = val;
}


// Calculate C flag value for additions.
bool Simulator::CarryFrom(int32_t left, int32_t right) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);
  uint32_t urest  = 0xffffffffU - uleft;

  return (uright > urest);
}


// Calculate C flag value for subtractions.
bool Simulator::BorrowFrom(int32_t left, int32_t right) {
  uint32_t uleft = static_cast<uint32_t>(left);
  uint32_t uright = static_cast<uint32_t>(right);

  return (uright > uleft);
}


// Calculate V flag value for additions and subtractions.
bool Simulator::OverflowFrom(int32_t alu_out,
                             int32_t left, int32_t right, bool addition) {
  bool overflow;
  if (addition) {
               // operands have the same sign
    overflow = ((left >= 0 && right >= 0) || (left < 0 && right < 0))
               // and operands and result have different sign
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  } else {
               // operands have different signs
    overflow = ((left < 0 && right >= 0) || (left >= 0 && right < 0))
               // and first operand and result have different signs
               && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
  }
  return overflow;
}


// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with register.
int32_t Simulator::GetShiftRm(Instr* instr, bool* carry_out) {
  Shift shift = instr->ShiftField();
  int shift_amount = instr->ShiftAmountField();
  int32_t result = get_register(instr->RmField());
  if (instr->Bit(4) == 0) {
    // by immediate
    if ((shift == ROR) && (shift_amount == 0)) {
      UnimplementedInstruction(instr);
    } else if (((shift == LSR) || (shift == ASR)) && (shift_amount == 0)) {
      shift_amount = 32;
    }
    switch (shift) {
      case ASR: {
        if (shift_amount == 0) {
          if (result < 0) {
            result = 0xffffffff;
            *carry_out = true;
          } else {
            result = 0;
            *carry_out = false;
          }
        } else {
          result >>= (shift_amount - 1);
          *carry_out = (result & 1) == 1;
          result >>= 1;
        }
        break;
      }

      case LSL: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else {
          result <<= (shift_amount - 1);
          *carry_out = (result < 0);
          result <<= 1;
        }
        break;
      }

      case LSR: {
        if (shift_amount == 0) {
          result = 0;
          *carry_out = c_flag_;
        } else {
          uint32_t uresult = static_cast<uint32_t>(result);
          uresult >>= (shift_amount - 1);
          *carry_out = (uresult & 1) == 1;
          uresult >>= 1;
          result = static_cast<int32_t>(uresult);
        }
        break;
      }

      case ROR: {
        UnimplementedInstruction(instr);
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  } else {
    // by register
    Register rs = instr->RsField();
    shift_amount = get_register(rs) &0xff;
    switch (shift) {
      case ASR: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          result >>= (shift_amount - 1);
          *carry_out = (result & 1) == 1;
          result >>= 1;
        } else {
          ASSERT(shift_amount >= 32);
          if (result < 0) {
            *carry_out = true;
            result = 0xffffffff;
          } else {
            *carry_out = false;
            result = 0;
          }
        }
        break;
      }

      case LSL: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          result <<= (shift_amount - 1);
          *carry_out = (result < 0);
          result <<= 1;
        } else if (shift_amount == 32) {
          *carry_out = (result & 1) == 1;
          result = 0;
        } else {
          ASSERT(shift_amount > 32);
          *carry_out = false;
          result = 0;
        }
        break;
      }

      case LSR: {
        if (shift_amount == 0) {
          *carry_out = c_flag_;
        } else if (shift_amount < 32) {
          uint32_t uresult = static_cast<uint32_t>(result);
          uresult >>= (shift_amount - 1);
          *carry_out = (uresult & 1) == 1;
          uresult >>= 1;
          result = static_cast<int32_t>(uresult);
        } else if (shift_amount == 32) {
          *carry_out = (result < 0);
          result = 0;
        } else {
          *carry_out = false;
          result = 0;
        }
        break;
      }

      case ROR: {
        UnimplementedInstruction(instr);
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  }
  return result;
}


// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with immediate.
int32_t Simulator::GetImm(Instr* instr, bool* carry_out) {
  int rotate = instr->RotateField() * 2;
  int immed8 = instr->Immed8Field();
  int imm = (immed8 >> rotate) | (immed8 << (32 - rotate));
  *carry_out = (rotate == 0) ? c_flag_ : (imm < 0);
  return imm;
}


static int count_bits(int bit_vector) {
  int count = 0;
  while (bit_vector != 0) {
    if ((bit_vector & 1) != 0) {
      count++;
    }
    bit_vector >>= 1;
  }
  return count;
}


// Addressing Mode 4 - Load and Store Multiple
void Simulator::HandleRList(Instr* instr, bool load) {
  Register rn = instr->RnField();
  int32_t rn_val = get_register(rn);
  int rlist = instr->RlistField();
  int num_regs = count_bits(rlist);

  uword address = 0;
  uword end_address = 0;
  switch (instr->PUField()) {
    case 0: {
      // Print("da");
      address = rn_val - (num_regs * 4) + 4;
      end_address = rn_val + 4;
      rn_val = rn_val - (num_regs * 4);
      break;
    }
    case 1: {
      // Print("ia");
      address = rn_val;
      end_address = rn_val + (num_regs * 4);
      rn_val = rn_val + (num_regs * 4);
      break;
    }
    case 2: {
      // Print("db");
      address = rn_val - (num_regs * 4);
      end_address = rn_val;
      rn_val = address;
      break;
    }
    case 3: {
      // Print("ib");
      address = rn_val + 4;
      end_address = rn_val + (num_regs * 4) + 4;
      rn_val = rn_val + (num_regs * 4);
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (IsIllegalAddress(address)) {
    HandleIllegalAccess(address, instr);
  } else {
    if (instr->HasW()) {
      set_register(rn, rn_val);
    }
    int reg = 0;
    while (rlist != 0) {
      if ((rlist & 1) != 0) {
        if (load) {
          set_register(static_cast<Register>(reg), ReadW(address, instr));
        } else {
          WriteW(address, get_register(static_cast<Register>(reg)), instr);
        }
        address += 4;
      }
      reg++;
      rlist >>= 1;
    }
    ASSERT(end_address == address);
  }
}


// Calls into the Dart runtime are based on this interface.
typedef void (*SimulatorRuntimeCall)(NativeArguments arguments);

// Calls to leaf Dart runtime functions are based on this interface.
typedef int32_t (*SimulatorLeafRuntimeCall)(
    int32_t r0, int32_t r1, int32_t r2, int32_t r3);

// Calls to native Dart functions are based on this interface.
typedef void (*SimulatorNativeCall)(NativeArguments* arguments);


void Simulator::SupervisorCall(Instr* instr) {
  int svc = instr->SvcField();
  switch (svc) {
    case kRedirectionSvcCode: {
      SimulatorSetjmpBuffer buffer(this);

      if (!setjmp(buffer.buffer_)) {
        int32_t saved_lr = get_register(LR);
        Redirection* redirection = Redirection::FromSvcInstruction(instr);
        uword external = redirection->external_function();
        if (FLAG_trace_sim) {
          OS::Print("Call to host function at 0x%"Pd"\n", external);
        }

        if (redirection->call_kind() != kLeafRuntimeCall) {
          // The top_exit_frame_info of the current isolate points to the top of
          // the simulator stack.
          ASSERT((StackTop() - Isolate::Current()->top_exit_frame_info()) <
                 Isolate::GetSpecifiedStackSize());
          // Set the top_exit_frame_info of this simulator to the native stack.
          set_top_exit_frame_info(reinterpret_cast<uword>(&buffer));
        }
        if (redirection->call_kind() == kRuntimeCall) {
          NativeArguments arguments;
          ASSERT(sizeof(NativeArguments) == 4*kWordSize);
          arguments.isolate_ = reinterpret_cast<Isolate*>(get_register(R0));
          arguments.argc_tag_ = get_register(R1);
          arguments.argv_ = reinterpret_cast<RawObject*(*)[]>(get_register(R2));
          arguments.retval_ = reinterpret_cast<RawObject**>(get_register(R3));
          SimulatorRuntimeCall target =
              reinterpret_cast<SimulatorRuntimeCall>(external);
          target(arguments);
          set_register(R0, icount_);  // Zap result register from void function.
        } else if (redirection->call_kind() == kLeafRuntimeCall) {
          int32_t r0 = get_register(R0);
          int32_t r1 = get_register(R1);
          int32_t r2 = get_register(R2);
          int32_t r3 = get_register(R3);
          SimulatorLeafRuntimeCall target =
              reinterpret_cast<SimulatorLeafRuntimeCall>(external);
          r0 = target(r0, r1, r2, r3);
          set_register(R0, r0);  // Set returned result from function.
        } else {
          ASSERT(redirection->call_kind() == kNativeCall);
          NativeArguments* arguments;
          arguments = reinterpret_cast<NativeArguments*>(get_register(R0));
          SimulatorNativeCall target =
              reinterpret_cast<SimulatorNativeCall>(external);
          target(arguments);
          set_register(R0, icount_);  // Zap result register from void function.
        }
        set_top_exit_frame_info(0);

        // Zap caller-saved registers, since the actual runtime call could have
        // used them.
        set_register(R1, icount_);
        set_register(R2, icount_);
        set_register(R3, icount_);
        set_register(IP, icount_);
        set_register(LR, icount_);
        float zap_fvalue = static_cast<float>(icount_);
        for (int i = S0; i <= S15; i++) {
          set_sregister(static_cast<SRegister>(i), zap_fvalue);
        }
#ifdef VFPv3_D32
        double zap_dvalue = static_cast<double>(icount_);
        for (int i = D16; i <= D31; i++) {
          set_dregister(static_cast<DRegister>(i), zap_dvalue);
        }
#endif  // VFPv3_D32

        // Return.
        set_pc(saved_lr);
      }

      break;
    }
    case kBreakpointSvcCode: {
      SimulatorDebugger dbg(this);
      dbg.Stop(instr, "breakpoint");
      break;
    }
    case kStopMessageSvcCode: {
      SimulatorDebugger dbg(this);
      const char* message = *reinterpret_cast<const char**>(
          reinterpret_cast<intptr_t>(instr) - Instr::kInstrSize);
      set_pc(get_pc() + Instr::kInstrSize);
      dbg.Stop(instr, message);
      break;
    }
    case kWordSpillMarkerSvcCode: {
      static StatsCounter counter_spill_w("Simulated word spills");
      counter_spill_w.Increment();
      break;
    }
    case kDWordSpillMarkerSvcCode: {
      static StatsCounter counter_spill_d("Simulated double word spills");
      counter_spill_d.Increment();
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
}


// Handle execution based on instruction types.

// Instruction types 0 and 1 are both rolled into one function because they
// only differ in the handling of the shifter_operand.
void Simulator::DecodeType01(Instr* instr) {
  if (!instr->IsDataProcessing()) {
    // miscellaneous, multiply, sync primitives, extra loads and stores.
    if (instr->IsMiscellaneous()) {
      switch (instr->Bits(4, 3)) {
        case 1: {
          if (instr->Bits(21, 2) == 0x3) {
            // Format(instr, "clz'cond 'rd, 'rm");
            Register rm = instr->RmField();
            Register rd = instr->RdField();
            int32_t rm_val = get_register(rm);
            int32_t rd_val = 0;
            if (rm_val != 0) {
              while (rm_val > 0) {
                rd_val++;
                rm_val <<= 1;
              }
            } else {
              rd_val = 32;
            }
            set_register(rd, rd_val);
          } else {
            ASSERT(instr->Bits(21, 2) == 0x1);
            // Format(instr, "bx'cond 'rm");
            Register rm = instr->RmField();
            int32_t rm_val = get_register(rm);
            set_pc(rm_val);
          }
          break;
        }
        case 3: {
          ASSERT(instr->Bits(21, 2) == 0x1);
          // Format(instr, "blx'cond 'rm");
          Register rm = instr->RmField();
          int32_t rm_val = get_register(rm);
          intptr_t pc = get_pc();
          set_register(LR, pc + Instr::kInstrSize);
          set_pc(rm_val);
          break;
        }
        case 7: {
          if (instr->Bits(21, 2) == 0x1) {
            // Format(instr, "bkpt'cond #'imm12_4");
            SimulatorDebugger dbg(this);
            set_pc(get_pc() + Instr::kInstrSize);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "bkpt #0x%x", instr->BkptField());
            dbg.Stop(instr, buffer);
          } else {
             // Format(instr, "smc'cond");
            UnimplementedInstruction(instr);
          }
          break;
        }
        default: {
          UnimplementedInstruction(instr);
          break;
        }
      }
    } else if (instr->IsMultiplyOrSyncPrimitive()) {
      if (instr->Bit(24) == 0) {
        // multiply instructions.
        Register rn = instr->RnField();
        Register rd = instr->RdField();
        Register rs = instr->RsField();
        Register rm = instr->RmField();
        int32_t rm_val = get_register(rm);
        int32_t rs_val = get_register(rs);
        int32_t rd_val = 0;
        switch (instr->Bits(21, 3)) {
          case 1:
            // Registers rd, rn, rm, ra are encoded as rn, rm, rs, rd.
            // Format(instr, "mla'cond's 'rn, 'rm, 'rs, 'rd");
          case 3: {
            // Registers rd, rn, rm, ra are encoded as rn, rm, rs, rd.
            // Format(instr, "mls'cond's 'rn, 'rm, 'rs, 'rd");
            rd_val = get_register(rd);
            // fall through
          }
          case 0: {
            // Registers rd, rn, rm are encoded as rn, rm, rs.
            // Format(instr, "mul'cond's 'rn, 'rm, 'rs");
            int32_t alu_out = rm_val * rs_val;
            if (instr->Bits(21, 3) == 3) {  // mls
              alu_out = -alu_out;
            }
            alu_out += rd_val;
            set_register(rn, alu_out);
            if (instr->HasS()) {
              SetNZFlags(alu_out);
            }
            break;
          }
          case 4: {
            // Registers rd_lo, rd_hi, rn, rm are encoded as rd, rn, rm, rs.
            // Format(instr, "umull'cond's 'rd, 'rn, 'rm, 'rs");
            uint64_t left_op  = static_cast<uint32_t>(rm_val);
            uint64_t right_op = static_cast<uint32_t>(rs_val);
            uint64_t result = left_op * right_op;
            int32_t hi_res = Utils::High32Bits(result);
            int32_t lo_res = Utils::Low32Bits(result);
            set_register(rd, lo_res);
            set_register(rn, hi_res);
            if (instr->HasS()) {
              if (lo_res != 0) {
                // Collapse bits 0..31 into bit 32 so that 32-bit Z check works.
                hi_res |= 1;
              }
              ASSERT((result == 0) == (hi_res == 0));  // Z bit
              ASSERT(((result & (1LL << 63)) != 0) == (hi_res < 0));  // N bit
              SetNZFlags(hi_res);
            }
            break;
          }
          default: {
            UnimplementedInstruction(instr);
            break;
          }
        }
      } else {
        // synchronization primitives
        Register rd = instr->RdField();
        Register rn = instr->RnField();
        uword addr = get_register(rn);
        switch (instr->Bits(20, 4)) {
          case 8: {
            // Format(instr, "strex'cond 'rd, 'rm, ['rn]");
            if (IsIllegalAddress(addr)) {
              HandleIllegalAccess(addr, instr);
            } else {
              Register rm = instr->RmField();
              set_register(rd, WriteExclusiveW(addr, get_register(rm), instr));
            }
            break;
          }
          case 9: {
            // Format(instr, "ldrex'cond 'rd, ['rn]");
            if (IsIllegalAddress(addr)) {
              HandleIllegalAccess(addr, instr);
            } else {
              set_register(rd, ReadExclusiveW(addr, instr));
            }
            break;
          }
          default: {
            UnimplementedInstruction(instr);
            break;
          }
        }
      }
    } else if (instr->Bit(25) == 1) {
      // 16-bit immediate loads, msr (immediate), and hints
      switch (instr->Bits(20, 5)) {
        case 16:
        case 20: {
          uint16_t imm16 = instr->MovwField();
          Register rd = instr->RdField();
          if (instr->Bit(22) == 0) {
            // Format(instr, "movw'cond 'rd, #'imm4_12");
            set_register(rd, imm16);
          } else {
            // Format(instr, "movt'cond 'rd, #'imm4_12");
            set_register(rd, (get_register(rd) & 0xffff) | (imm16 << 16));
          }
          break;
        }
        case 18: {
          if ((instr->Bits(16, 4) == 0) && (instr->Bits(0, 8) == 0)) {
            // Format(instr, "nop'cond");
          } else {
            UnimplementedInstruction(instr);
          }
          break;
        }
        default: {
          UnimplementedInstruction(instr);
          break;
        }
      }
    } else {
      // extra load/store instructions
      Register rd = instr->RdField();
      Register rn = instr->RnField();
      int32_t rn_val = get_register(rn);
      uword addr = 0;
      bool write_back = false;
      if (instr->Bit(22) == 0) {
        Register rm = instr->RmField();
        int32_t rm_val = get_register(rm);
        switch (instr->PUField()) {
          case 0: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn], -'rm");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val -= rm_val;
            write_back = true;
            break;
          }
          case 1: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn], +'rm");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val += rm_val;
            write_back = true;
            break;
          }
          case 2: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn, -'rm]'w");
            rn_val -= rm_val;
            addr = rn_val;
            write_back = instr->HasW();
            break;
          }
          case 3: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn, +'rm]'w");
            rn_val += rm_val;
            addr = rn_val;
            write_back = instr->HasW();
            break;
          }
          default: {
            // The PU field is a 2-bit field.
            UNREACHABLE();
            break;
          }
        }
      } else {
        int32_t imm_val = (instr->ImmedHField() << 4) | instr->ImmedLField();
        switch (instr->PUField()) {
          case 0: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn], #-'off8");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val -= imm_val;
            write_back = true;
            break;
          }
          case 1: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn], #+'off8");
            ASSERT(!instr->HasW());
            addr = rn_val;
            rn_val += imm_val;
            write_back = true;
            break;
          }
          case 2: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn, #-'off8]'w");
            rn_val -= imm_val;
            addr = rn_val;
            write_back = instr->HasW();
            break;
          }
          case 3: {
            // Format(instr, "'memop'cond'x 'rd2, ['rn, #+'off8]'w");
            rn_val += imm_val;
            addr = rn_val;
            write_back = instr->HasW();
            break;
          }
          default: {
            // The PU field is a 2-bit field.
            UNREACHABLE();
            break;
          }
        }
      }
      if (IsIllegalAddress(addr)) {
        HandleIllegalAccess(addr, instr);
      } else {
        if (write_back) {
          set_register(rn, rn_val);
        }
        if (!instr->HasSign()) {
          if (instr->HasL()) {
            uint16_t val = ReadHU(addr, instr);
            set_register(rd, val);
          } else {
            uint16_t val = get_register(rd);
            WriteH(addr, val, instr);
          }
        } else if (instr->HasL()) {
          if (instr->HasH()) {
            int16_t val = ReadH(addr, instr);
            set_register(rd, val);
          } else {
            int8_t val = ReadB(addr);
            set_register(rd, val);
          }
        } else if ((rd & 1) == 0) {
          Register rd1 = static_cast<Register>(rd | 1);
          ASSERT(rd1 < kNumberOfCpuRegisters);
          if (instr->HasH()) {
            int32_t val_low = get_register(rd);
            int32_t val_high = get_register(rd1);
            WriteW(addr, val_low, instr);
            WriteW(addr + 4, val_high, instr);
          } else {
            int32_t val_low = ReadW(addr, instr);
            int32_t val_high = ReadW(addr + 4, instr);
            set_register(rd, val_low);
            set_register(rd1, val_high);
          }
        } else {
          UnimplementedInstruction(instr);
        }
      }
    }
  } else {
    Register rd = instr->RdField();
    Register rn = instr->RnField();
    int32_t rn_val = get_register(rn);
    int32_t shifter_operand = 0;
    bool shifter_carry_out = 0;
    if (instr->TypeField() == 0) {
      shifter_operand = GetShiftRm(instr, &shifter_carry_out);
    } else {
      ASSERT(instr->TypeField() == 1);
      shifter_operand = GetImm(instr, &shifter_carry_out);
    }
    int32_t alu_out;

    switch (instr->OpcodeField()) {
      case AND: {
        // Format(instr, "and'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "and'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val & shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case EOR: {
        // Format(instr, "eor'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "eor'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val ^ shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case SUB: {
        // Format(instr, "sub'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "sub'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val - shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, false));
        }
        break;
      }

      case RSB: {
        // Format(instr, "rsb'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "rsb'cond's 'rd, 'rn, 'imm");
        alu_out = shifter_operand - rn_val;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(shifter_operand, rn_val));
          SetVFlag(OverflowFrom(alu_out, shifter_operand, rn_val, false));
        }
        break;
      }

      case ADD: {
        // Format(instr, "add'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "add'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val + shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        }
        break;
      }

      case ADC: {
        // Format(instr, "adc'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "adc'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val + shifter_operand + (c_flag_ ? 1 : 0);
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        }
        break;
      }

      case SBC: {
        // Format(instr, "sbc'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "sbc'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val - shifter_operand - (!c_flag_ ? 1 : 0);
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, false));
        }
        break;
      }

      case RSC: {
        // Format(instr, "rsc'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "rsc'cond's 'rd, 'rn, 'imm");
        alu_out = shifter_operand - rn_val - (!c_flag_ ? 1 : 0);
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(shifter_operand, rn_val));
          SetVFlag(OverflowFrom(alu_out, shifter_operand, rn_val, false));
        }
        break;
      }

      case TST: {
        if (instr->HasS()) {
          // Format(instr, "tst'cond 'rn, 'shift_rm");
          // Format(instr, "tst'cond 'rn, 'imm");
          alu_out = rn_val & shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        } else {
          UnimplementedInstruction(instr);
        }
        break;
      }

      case TEQ: {
        if (instr->HasS()) {
          // Format(instr, "teq'cond 'rn, 'shift_rm");
          // Format(instr, "teq'cond 'rn, 'imm");
          alu_out = rn_val ^ shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        } else {
          UnimplementedInstruction(instr);
        }
        break;
      }

      case CMP: {
        if (instr->HasS()) {
          // Format(instr, "cmp'cond 'rn, 'shift_rm");
          // Format(instr, "cmp'cond 'rn, 'imm");
          alu_out = rn_val - shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(!BorrowFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, false));
        } else {
          UnimplementedInstruction(instr);
        }
        break;
      }

      case CMN: {
        if (instr->HasS()) {
          // Format(instr, "cmn'cond 'rn, 'shift_rm");
          // Format(instr, "cmn'cond 'rn, 'imm");
          alu_out = rn_val + shifter_operand;
          SetNZFlags(alu_out);
          SetCFlag(CarryFrom(rn_val, shifter_operand));
          SetVFlag(OverflowFrom(alu_out, rn_val, shifter_operand, true));
        } else {
          UnimplementedInstruction(instr);
        }
        break;
      }

      case ORR: {
        // Format(instr, "orr'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "orr'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val | shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case MOV: {
        // Format(instr, "mov'cond's 'rd, 'shift_rm");
        // Format(instr, "mov'cond's 'rd, 'imm");
        alu_out = shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case BIC: {
        // Format(instr, "bic'cond's 'rd, 'rn, 'shift_rm");
        // Format(instr, "bic'cond's 'rd, 'rn, 'imm");
        alu_out = rn_val & ~shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      case MVN: {
        // Format(instr, "mvn'cond's 'rd, 'shift_rm");
        // Format(instr, "mvn'cond's 'rd, 'imm");
        alu_out = ~shifter_operand;
        set_register(rd, alu_out);
        if (instr->HasS()) {
          SetNZFlags(alu_out);
          SetCFlag(shifter_carry_out);
        }
        break;
      }

      default: {
        UNREACHABLE();
        break;
      }
    }
  }
}


void Simulator::DecodeType2(Instr* instr) {
  Register rd = instr->RdField();
  Register rn = instr->RnField();
  int32_t rn_val = get_register(rn);
  int32_t im_val = instr->Offset12Field();
  uword addr = 0;
  bool write_back = false;
  switch (instr->PUField()) {
    case 0: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], #-'off12");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val -= im_val;
      write_back = true;
      break;
    }
    case 1: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], #+'off12");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val += im_val;
      write_back = true;
      break;
    }
    case 2: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, #-'off12]'w");
      rn_val -= im_val;
      addr = rn_val;
      write_back = instr->HasW();
      break;
    }
    case 3: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, #+'off12]'w");
      rn_val += im_val;
      addr = rn_val;
      write_back = instr->HasW();
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (IsIllegalAddress(addr)) {
    HandleIllegalAccess(addr, instr);
  } else {
    if (write_back) {
      set_register(rn, rn_val);
    }
    if (instr->HasB()) {
      if (instr->HasL()) {
        unsigned char val = ReadBU(addr);
        set_register(rd, val);
      } else {
        unsigned char val = get_register(rd);
        WriteB(addr, val);
      }
    } else {
      if (instr->HasL()) {
        set_register(rd, ReadW(addr, instr));
      } else {
        WriteW(addr, get_register(rd), instr);
      }
    }
  }
}


void Simulator::DoDivision(Instr* instr) {
  ASSERT(CPUFeatures::integer_division_supported());
  Register rd = instr->RdField();
  Register rn = instr->RnField();
  Register rm = instr->RmField();

  // TODO(zra): Does the hardware trap on divide-by-zero?
  //     Revisit when we test on ARM hardware.
  if (get_register(rm) == 0) {
    set_register(rd, 0);
    return;
  }

  if (instr->Bit(21) == 1) {
    // unsigned division.
    uint32_t rn_val = static_cast<uint32_t>(get_register(rn));
    uint32_t rm_val = static_cast<uint32_t>(get_register(rm));
    uint32_t result = rn_val / rm_val;
    set_register(rd, static_cast<int32_t>(result));
  } else {
    // signed division.
    int32_t rn_val = get_register(rn);
    int32_t rm_val = get_register(rm);
    int32_t result;
    if ((rn_val == static_cast<int32_t>(0x80000000)) &&
        (rm_val == static_cast<int32_t>(0xffffffff))) {
      result = 0x80000000;
    } else {
      result = rn_val / rm_val;
    }
    set_register(rd, result);
  }
}


void Simulator::DecodeType3(Instr* instr) {
  if (instr->IsDivision()) {
    DoDivision(instr);
    return;
  }
  Register rd = instr->RdField();
  Register rn = instr->RnField();
  int32_t rn_val = get_register(rn);
  bool shifter_carry_out = 0;
  int32_t shifter_operand = GetShiftRm(instr, &shifter_carry_out);
  uword addr = 0;
  bool write_back = false;
  switch (instr->PUField()) {
    case 0: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], -'shift_rm");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val -= shifter_operand;
      write_back = true;
      break;
    }
    case 1: {
      // Format(instr, "'memop'cond'b 'rd, ['rn], +'shift_rm");
      ASSERT(!instr->HasW());
      addr = rn_val;
      rn_val += shifter_operand;
      write_back = true;
      break;
    }
    case 2: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, -'shift_rm]'w");
      rn_val -= shifter_operand;
      addr = rn_val;
      write_back = instr->HasW();
      break;
    }
    case 3: {
      // Format(instr, "'memop'cond'b 'rd, ['rn, +'shift_rm]'w");
      rn_val += shifter_operand;
      addr = rn_val;
      write_back = instr->HasW();
      break;
    }
    default: {
      UNREACHABLE();
      break;
    }
  }
  if (IsIllegalAddress(addr)) {
    HandleIllegalAccess(addr, instr);
  } else {
    if (write_back) {
      set_register(rn, rn_val);
    }
    if (instr->HasB()) {
      if (instr->HasL()) {
        unsigned char val = ReadBU(addr);
        set_register(rd, val);
      } else {
        unsigned char val = get_register(rd);
        WriteB(addr, val);
      }
    } else {
      if (instr->HasL()) {
        set_register(rd, ReadW(addr, instr));
      } else {
        WriteW(addr, get_register(rd), instr);
      }
    }
  }
}


void Simulator::DecodeType4(Instr* instr) {
  ASSERT(instr->Bit(22) == 0);  // only allowed to be set in privileged mode
  if (instr->HasL()) {
    // Format(instr, "ldm'cond'pu 'rn'w, 'rlist");
    HandleRList(instr, true);
  } else {
    // Format(instr, "stm'cond'pu 'rn'w, 'rlist");
    HandleRList(instr, false);
  }
}


void Simulator::DecodeType5(Instr* instr) {
  // Format(instr, "b'l'cond 'target");
  int off = (instr->SImmed24Field() << 2) + 8;
  intptr_t pc = get_pc();
  if (instr->HasLink()) {
    set_register(LR, pc + Instr::kInstrSize);
  }
  set_pc(pc+off);
}


void Simulator::DecodeType6(Instr* instr) {
  if (instr->IsVFPDoubleTransfer()) {
    Register rd = instr->RdField();
    Register rn = instr->RnField();
    if (instr->Bit(8) == 0) {
      SRegister sm = instr->SmField();
      SRegister sm1 = static_cast<SRegister>(sm + 1);
      ASSERT(sm1 < kNumberOfSRegisters);
      if (instr->Bit(20) == 1) {
        // Format(instr, "vmovrrs'cond 'rd, 'rn, {'sm', 'sm1}");
        set_register(rd, bit_cast<int32_t, float>(get_sregister(sm)));
        set_register(rn, bit_cast<int32_t, float>(get_sregister(sm1)));
      } else {
        // Format(instr, "vmovsrr'cond {'sm, 'sm1}, 'rd', 'rn");
        set_sregister(sm, bit_cast<float, int32_t>(get_register(rd)));
        set_sregister(sm1, bit_cast<float, int32_t>(get_register(rn)));
      }
    } else {
      DRegister dm = instr->DmField();
      if (instr->Bit(20) == 1) {
        // Format(instr, "vmovrrd'cond 'rd, 'rn, 'dm");
        int64_t dm_val = bit_cast<int64_t, double>(get_dregister(dm));
        set_register(rd, Utils::Low32Bits(dm_val));
        set_register(rn, Utils::High32Bits(dm_val));
      } else {
        // Format(instr, "vmovdrr'cond 'dm, 'rd, 'rn");
        int64_t dm_val = Utils::LowHighTo64Bits(get_register(rd),
                                                get_register(rn));
        set_dregister(dm, bit_cast<double, int64_t>(dm_val));
      }
    }
  } else if (instr-> IsVFPLoadStore()) {
    Register rn = instr->RnField();
    int32_t addr = get_register(rn);
    int32_t imm_val = instr->Bits(0, 8) << 2;
    if (instr->Bit(23) == 1) {
      addr += imm_val;
    } else {
      addr -= imm_val;
    }
    if (IsIllegalAddress(addr)) {
      HandleIllegalAccess(addr, instr);
    } else {
      if (instr->Bit(8) == 0) {
        SRegister sd = instr->SdField();
        if (instr->Bit(20) == 1) {  // vldrs
          // Format(instr, "vldrs'cond 'sd, ['rn, #+'off10]");
          // Format(instr, "vldrs'cond 'sd, ['rn, #-'off10]");
          set_sregister(sd, bit_cast<float, int32_t>(ReadW(addr, instr)));
        } else {  // vstrs
          // Format(instr, "vstrs'cond 'sd, ['rn, #+'off10]");
          // Format(instr, "vstrs'cond 'sd, ['rn, #-'off10]");
          WriteW(addr, bit_cast<int32_t, float>(get_sregister(sd)), instr);
        }
      } else {
        DRegister dd = instr->DdField();
        if (instr->Bit(20) == 1) {  // vldrd
          // Format(instr, "vldrd'cond 'dd, ['rn, #+'off10]");
          // Format(instr, "vldrd'cond 'dd, ['rn, #-'off10]");
          int64_t dd_val = Utils::LowHighTo64Bits(ReadW(addr, instr),
                                                  ReadW(addr + 4, instr));
          set_dregister(dd, bit_cast<double, int64_t>(dd_val));
        } else {  // vstrd
          // Format(instr, "vstrd'cond 'dd, ['rn, #+'off10]");
          // Format(instr, "vstrd'cond 'dd, ['rn, #-'off10]");
          int64_t dd_val = bit_cast<int64_t, double>(get_dregister(dd));
          WriteW(addr, Utils::Low32Bits(dd_val), instr);
          WriteW(addr + 4, Utils::High32Bits(dd_val), instr);
        }
      }
    }
  } else if (instr->IsVFPMultipleLoadStore()) {
    Register rn = instr->RnField();
    int32_t addr = get_register(rn);
    int32_t imm_val = instr->Bits(0, 8);
    if (instr->Bit(23) == 0) {
      addr -= (imm_val << 2);
    }
    if (instr->HasW()) {
      if (instr->Bit(23) == 1) {
        set_register(rn, addr + (imm_val << 2));
      } else {
        set_register(rn, addr);  // already subtracted from addr
      }
    }
    if (IsIllegalAddress(addr)) {
      HandleIllegalAccess(addr, instr);
    } else {
      if (instr->Bit(8) == 0) {
        int32_t regs_cnt = imm_val;
        int32_t start = instr->Bit(22) | (instr->Bits(12, 4) << 1);
        for (int i = start; i < start + regs_cnt; i++) {
          SRegister sd = static_cast<SRegister>(i);
          if (instr->Bit(20) == 1) {
            // Format(instr, "vldms'cond'pu 'rn'w, 'slist");
            set_sregister(sd, bit_cast<float, int32_t>(ReadW(addr, instr)));
          } else {
            // Format(instr, "vstms'cond'pu 'rn'w, 'slist");
            WriteW(addr, bit_cast<int32_t, float>(get_sregister(sd)), instr);
          }
          addr += 4;
        }
      } else {
        int32_t regs_cnt = imm_val >> 1;
        int32_t start = (instr->Bit(22) << 4) | instr->Bits(12, 4);
        for (int i = start; i < start + regs_cnt; i++) {
          DRegister dd = static_cast<DRegister>(i);
          if (instr->Bit(20) == 1) {
            // Format(instr, "vldmd'cond'pu 'rn'w, 'dlist");
            int64_t dd_val = Utils::LowHighTo64Bits(ReadW(addr, instr),
                                                    ReadW(addr + 4, instr));
            set_dregister(dd, bit_cast<double, int64_t>(dd_val));
          } else {
            // Format(instr, "vstmd'cond'pu 'rn'w, 'dlist");
            int64_t dd_val = bit_cast<int64_t, double>(get_dregister(dd));
            WriteW(addr, Utils::Low32Bits(dd_val), instr);
            WriteW(addr + 4, Utils::High32Bits(dd_val), instr);
          }
          addr += 8;
        }
      }
    }
  } else {
    UnimplementedInstruction(instr);
  }
}


void Simulator::DecodeType7(Instr* instr) {
  if (instr->Bit(24) == 1) {
    // Format(instr, "svc #'svc");
    SupervisorCall(instr);
  } else if (instr->IsVFPDataProcessingOrSingleTransfer()) {
    if (instr->Bit(4) == 0) {
      // VFP Data Processing
      SRegister sd;
      SRegister sn;
      SRegister sm;
      DRegister dd;
      DRegister dn;
      DRegister dm;
      if (instr->Bit(8) == 0) {
        sd = instr->SdField();
        sn = instr->SnField();
        sm = instr->SmField();
        dd = kNoDRegister;
        dn = kNoDRegister;
        dm = kNoDRegister;
      } else {
        sd = kNoSRegister;
        sn = kNoSRegister;
        sm = kNoSRegister;
        dd = instr->DdField();
        dn = instr->DnField();
        dm = instr->DmField();
      }
      switch (instr->Bits(20, 4) & 0xb) {
        case 1:  // vnmla, vnmls, vnmul
        default: {
          UnimplementedInstruction(instr);
          break;
        }
        case 0: {  // vmla, vmls floating-point
          if (instr->Bit(8) == 0) {
            float addend = get_sregister(sn) * get_sregister(sm);
            float sd_val = get_sregister(sd);
            if (instr->Bit(6) == 0) {
              // Format(instr, "vmlas'cond 'sd, 'sn, 'sm");
            } else {
              // Format(instr, "vmlss'cond 'sd, 'sn, 'sm");
              addend = -addend;
            }
            set_sregister(sd, sd_val + addend);
          } else {
            double addend = get_dregister(dn) * get_dregister(dm);
            double dd_val = get_dregister(dd);
            if (instr->Bit(6) == 0) {
              // Format(instr, "vmlad'cond 'dd, 'dn, 'dm");
            } else {
              // Format(instr, "vmlsd'cond 'dd, 'dn, 'dm");
              addend = -addend;
            }
            set_dregister(dd, dd_val + addend);
          }
          break;
        }
        case 2: {  // vmul
          if (instr->Bit(8) == 0) {
            // Format(instr, "vmuls'cond 'sd, 'sn, 'sm");
            set_sregister(sd, get_sregister(sn) * get_sregister(sm));
          } else {
            // Format(instr, "vmuld'cond 'dd, 'dn, 'dm");
            set_dregister(dd, get_dregister(dn) * get_dregister(dm));
          }
          break;
        }
        case 8: {  // vdiv
          if (instr->Bit(8) == 0) {
            // Format(instr, "vdivs'cond 'sd, 'sn, 'sm");
            set_sregister(sd, get_sregister(sn) / get_sregister(sm));
          } else {
            // Format(instr, "vdivd'cond 'dd, 'dn, 'dm");
            set_dregister(dd, get_dregister(dn) / get_dregister(dm));
          }
          break;
        }
        case 3: {  // vadd, vsub floating-point
          if (instr->Bit(8) == 0) {
            if (instr->Bit(6) == 0) {
              // Format(instr, "vadds'cond 'sd, 'sn, 'sm");
              set_sregister(sd, get_sregister(sn) + get_sregister(sm));
            } else {
              // Format(instr, "vsubs'cond 'sd, 'sn, 'sm");
              set_sregister(sd, get_sregister(sn) - get_sregister(sm));
            }
          } else {
            if (instr->Bit(6) == 0) {
              // Format(instr, "vaddd'cond 'dd, 'dn, 'dm");
              set_dregister(dd, get_dregister(dn) + get_dregister(dm));
            } else {
              // Format(instr, "vsubd'cond 'dd, 'dn, 'dm");
              set_dregister(dd, get_dregister(dn) - get_dregister(dm));
            }
          }
          break;
        }
        case 0xb: {  // Other VFP data-processing instructions
          if (instr->Bit(6) == 0) {  // vmov immediate
            if (instr->Bit(8) == 0) {
              // Format(instr, "vmovs'cond 'sd, #'immf");
              set_sregister(sd, instr->ImmFloatField());
            } else {
              // Format(instr, "vmovd'cond 'dd, #'immd");
              set_dregister(dd, instr->ImmDoubleField());
            }
            break;
          }
          switch (instr->Bits(16, 4)) {
            case 0: {  // vmov immediate, vmov register, vabs
              switch (instr->Bits(6, 2)) {
                case 1: {  // vmov register
                  if (instr->Bit(8) == 0) {
                    // Format(instr, "vmovs'cond 'sd, 'sm");
                    set_sregister(sd, get_sregister(sm));
                  } else {
                    // Format(instr, "vmovd'cond 'dd, 'dm");
                    set_dregister(dd, get_dregister(dm));
                  }
                  break;
                }
                case 3: {  // vabs
                  if (instr->Bit(8) == 0) {
                    // Format(instr, "vabss'cond 'sd, 'sm");
                    set_sregister(sd, fabsf(get_sregister(sm)));
                  } else {
                    // Format(instr, "vabsd'cond 'dd, 'dm");
                    set_dregister(dd, fabs(get_dregister(dm)));
                  }
                  break;
                }
                default: {
                  UnimplementedInstruction(instr);
                  break;
                }
              }
              break;
            }
            case 1: {  // vneg, vsqrt
              switch (instr->Bits(6, 2)) {
                case 1: {  // vneg
                  if (instr->Bit(8) == 0) {
                    // Format(instr, "vnegs'cond 'sd, 'sm");
                    set_sregister(sd, -get_sregister(sm));
                  } else {
                    // Format(instr, "vnegd'cond 'dd, 'dm");
                    set_dregister(dd, -get_dregister(dm));
                  }
                  break;
                }
                case 3: {  // vsqrt
                  if (instr->Bit(8) == 0) {
                    // Format(instr, "vsqrts'cond 'sd, 'sm");
                    set_sregister(sd, sqrtf(get_sregister(sm)));
                  } else {
                    // Format(instr, "vsqrtd'cond 'dd, 'dm");
                    set_dregister(dd, sqrt(get_dregister(dm)));
                  }
                  break;
                }
                default: {
                  UnimplementedInstruction(instr);
                  break;
                }
              }
              break;
            }
            case 4:  // vcmp, vcmpe
            case 5: {  // vcmp #0.0, vcmpe #0.0
              if (instr->Bit(7) == 1) {  // vcmpe
                UnimplementedInstruction(instr);
              } else {
                fp_n_flag_ = false;
                fp_z_flag_ = false;
                fp_c_flag_ = false;
                fp_v_flag_ = false;
                if (instr->Bit(8) == 0) {  // vcmps
                  float sd_val = get_sregister(sd);
                  float sm_val;
                  if (instr->Bit(16) == 0) {
                    // Format(instr, "vcmps'cond 'sd, 'sm");
                    sm_val = get_sregister(sm);
                  } else {
                    // Format(instr, "vcmps'cond 'sd, #0.0");
                    sm_val = 0.0f;
                  }
                  if (isnan(sd_val) || isnan(sm_val)) {
                    fp_c_flag_ = true;
                    fp_v_flag_ = true;
                  } else if (sd_val == sm_val) {
                    fp_z_flag_ = true;
                    fp_c_flag_ = true;
                  } else if (sd_val < sm_val) {
                    fp_n_flag_ = true;
                  } else {
                    fp_c_flag_ = true;
                  }
                } else {  // vcmpd
                  double dd_val = get_dregister(dd);
                  double dm_val;
                  if (instr->Bit(16) == 0) {
                    // Format(instr, "vcmpd'cond 'dd, 'dm");
                    dm_val = get_dregister(dm);
                  } else {
                    // Format(instr, "vcmpd'cond 'dd, #0.0");
                    dm_val = 0.0;
                  }
                  if (isnan(dd_val) || isnan(dm_val)) {
                    fp_c_flag_ = true;
                    fp_v_flag_ = true;
                  } else if (dd_val == dm_val) {
                    fp_z_flag_ = true;
                    fp_c_flag_ = true;
                  } else if (dd_val < dm_val) {
                    fp_n_flag_ = true;
                  } else {
                    fp_c_flag_ = true;
                  }
                }
              }
              break;
            }
            case 7: {  // vcvt between double-precision and single-precision
              if (instr->Bit(8) == 0) {
                // Format(instr, "vcvtds'cond 'dd, 'sm");
                dd = instr->DdField();
                set_dregister(dd, static_cast<double>(get_sregister(sm)));
              } else {
                // Format(instr, "vcvtsd'cond 'sd, 'dm");
                sd = instr->SdField();
                set_sregister(sd, static_cast<float>(get_dregister(dm)));
              }
              break;
            }
            case 8: {  // vcvt, vcvtr between floating-point and integer
              sm = instr->SmField();
              float sm_val = get_sregister(sm);
              uint32_t ud_val = 0;
              int32_t id_val = 0;
              if (instr->Bit(7) == 0) {  // vcvtsu, vcvtdu
                ud_val = bit_cast<uint32_t, float>(sm_val);
              } else {  // vcvtsi, vcvtdi
                id_val = bit_cast<int32_t, float>(sm_val);
              }
              if (instr->Bit(8) == 0) {
                float sd_val;
                if (instr->Bit(7) == 0) {
                  // Format(instr, "vcvtsu'cond 'sd, 'sm");
                  sd_val = static_cast<float>(ud_val);
                } else {
                  // Format(instr, "vcvtsi'cond 'sd, 'sm");
                  sd_val = static_cast<float>(id_val);
                }
                set_sregister(sd, sd_val);
              } else {
                double dd_val;
                if (instr->Bit(7) == 0) {
                  // Format(instr, "vcvtdu'cond 'dd, 'sm");
                  dd_val = static_cast<double>(ud_val);
                } else {
                  // Format(instr, "vcvtdi'cond 'dd, 'sm");
                  dd_val = static_cast<double>(id_val);
                }
                set_dregister(dd, dd_val);
              }
              break;
            }
            case 12:
            case 13: {  // vcvt, vcvtr between floating-point and integer
              // We do not need to record exceptions in the FPSCR cumulative
              // flags, because we do not use them.
              if (instr->Bit(7) == 0) {
                // We only support round-to-zero mode
                UnimplementedInstruction(instr);
                break;
              }
              int32_t id_val = 0;
              uint32_t ud_val = 0;
              if (instr->Bit(8) == 0) {
                float sm_val = get_sregister(sm);
                if (instr->Bit(16) == 0) {
                  // Format(instr, "vcvtus'cond 'sd, 'sm");
                  if (sm_val >= INT_MAX) {
                    ud_val = INT_MAX;
                  } else if (sm_val > 0.0) {
                    ud_val = static_cast<uint32_t>(sm_val);
                  }
                } else {
                  // Format(instr, "vcvtis'cond 'sd, 'sm");
                  if (sm_val <= INT_MIN) {
                    id_val = INT_MIN;
                  } else if (sm_val >= INT_MAX) {
                    id_val = INT_MAX;
                  } else {
                    id_val = static_cast<int32_t>(sm_val);
                  }
                  ASSERT((id_val >= 0) || !(sm_val >= 0.0));
                }
              } else {
                sd = instr->SdField();
                double dm_val = get_dregister(dm);
                if (instr->Bit(16) == 0) {
                  // Format(instr, "vcvtud'cond 'sd, 'dm");
                  if (dm_val >= INT_MAX) {
                    ud_val = INT_MAX;
                  } else if (dm_val > 0.0) {
                    ud_val = static_cast<uint32_t>(dm_val);
                  }
                } else {
                  // Format(instr, "vcvtid'cond 'sd, 'dm");
                  if (dm_val <= INT_MIN) {
                    id_val = INT_MIN;
                  } else if (dm_val >= INT_MAX) {
                    id_val = INT_MAX;
                  } else {
                    id_val = static_cast<int32_t>(dm_val);
                  }
                  ASSERT((id_val >= 0) || !(dm_val >= 0.0));
                }
              }
              float sd_val;
              if (instr->Bit(16) == 0) {
                sd_val = bit_cast<float, uint32_t>(ud_val);
              } else {
                sd_val = bit_cast<float, int32_t>(id_val);
              }
              set_sregister(sd, sd_val);
              break;
            }
            case 2:  // vcvtb, vcvtt
            case 3:  // vcvtb, vcvtt
            case 9:  // undefined
            case 10:  // vcvt between floating-point and fixed-point
            case 11:  // vcvt between floating-point and fixed-point
            case 14:  // vcvt between floating-point and fixed-point
            case 15:  // vcvt between floating-point and fixed-point
            default: {
              UnimplementedInstruction(instr);
              break;
            }
          }
        }
        break;
      }
    } else {
      // 8, 16, or 32-bit Transfer between ARM Core and VFP
      if ((instr->Bits(21, 3) == 0) && (instr->Bit(8) == 0)) {
        Register rd = instr->RdField();
        SRegister sn = instr->SnField();
        if (instr->Bit(20) == 0) {
          // Format(instr, "vmovs'cond 'sn, 'rd");
          set_sregister(sn, bit_cast<float, int32_t>(get_register(rd)));
        } else {
          // Format(instr, "vmovr'cond 'rd, 'sn");
          set_register(rd, bit_cast<int32_t, float>(get_sregister(sn)));
        }
      } else if ((instr->Bits(20, 4) == 0xf) && (instr->Bit(8) == 0) &&
                 (instr->Bits(12, 4) == 0xf)) {
        // Format(instr, "vmstat'cond");
        n_flag_ = fp_n_flag_;
        z_flag_ = fp_z_flag_;
        c_flag_ = fp_c_flag_;
        v_flag_ = fp_v_flag_;
      } else {
        UnimplementedInstruction(instr);
      }
    }
  } else if (instr->IsMrcIdIsar0()) {
    // mrc of ID_ISAR0.
    Register rd = instr->RdField();
    if (CPUFeatures::integer_division_supported()) {
      set_register(rd, 0x02100010);  // sim has sdiv, udiv, bkpt and clz.
    } else {
      set_register(rd, 0x00100010);  // simulator has only bkpt and clz.
    }
  } else {
    UnimplementedInstruction(instr);
  }
}


// Executes the current instruction.
void Simulator::InstructionDecode(Instr* instr) {
  pc_modified_ = false;
  if (FLAG_trace_sim) {
    const uword start = reinterpret_cast<uword>(instr);
    const uword end = start + Instr::kInstrSize;
    Disassembler::Disassemble(start, end);
  }
  if (instr->ConditionField() == kSpecialCondition) {
    if (instr->InstructionBits() == static_cast<int32_t>(0xf57ff01f)) {
      // Format(instr, "clrex");
      ClearExclusive();
    } else {
      UnimplementedInstruction(instr);
    }
  } else if (ConditionallyExecute(instr)) {
    switch (instr->TypeField()) {
      case 0:
      case 1: {
        DecodeType01(instr);
        break;
      }
      case 2: {
        DecodeType2(instr);
        break;
      }
      case 3: {
        DecodeType3(instr);
        break;
      }
      case 4: {
        DecodeType4(instr);
        break;
      }
      case 5: {
        DecodeType5(instr);
        break;
      }
      case 6: {
        DecodeType6(instr);
        break;
      }
      case 7: {
        DecodeType7(instr);
        break;
      }
      default: {
        // Type field is three bits.
        UNREACHABLE();
        break;
      }
    }
  }
  if (!pc_modified_) {
    set_register(PC, reinterpret_cast<int32_t>(instr) + Instr::kInstrSize);
  }
}


void Simulator::Execute() {
  static StatsCounter counter_instructions("Simulated instructions");

  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  uword program_counter = get_pc();

  if (FLAG_stop_sim_at == 0) {
    // Fast version of the dispatch loop without checking whether the simulator
    // should be stopping at a particular executed instruction.
    while (program_counter != kEndSimulatingPC) {
      Instr* instr = reinterpret_cast<Instr*>(program_counter);
      icount_++;
      counter_instructions.Increment();
      if (IsIllegalAddress(program_counter)) {
        HandleIllegalAccess(program_counter, instr);
      } else {
        InstructionDecode(instr);
      }
      program_counter = get_pc();
    }
  } else {
    // FLAG_stop_sim_at is at the non-default value. Stop in the debugger when
    // we reach the particular instruction count.
    while (program_counter != kEndSimulatingPC) {
      Instr* instr = reinterpret_cast<Instr*>(program_counter);
      icount_++;
      counter_instructions.Increment();
      if (icount_ == FLAG_stop_sim_at) {
        SimulatorDebugger dbg(this);
        dbg.Stop(instr, "Instruction count reached");
      } else if (IsIllegalAddress(program_counter)) {
        HandleIllegalAccess(program_counter, instr);
      } else {
        InstructionDecode(instr);
      }
      program_counter = get_pc();
    }
  }
}


int64_t Simulator::Call(int32_t entry,
                        int32_t parameter0,
                        int32_t parameter1,
                        int32_t parameter2,
                        int32_t parameter3) {
  // Save the SP register before the call so we can restore it.
  int32_t sp_before_call = get_register(SP);

  // Setup parameters.
  set_register(R0, parameter0);
  set_register(R1, parameter1);
  set_register(R2, parameter2);
  set_register(R3, parameter3);

  // Make sure the activation frames are properly aligned.
  int32_t stack_pointer = sp_before_call;
  static const int kFrameAlignment = OS::ActivationFrameAlignment();
  if (kFrameAlignment > 0) {
    stack_pointer = Utils::RoundDown(stack_pointer, kFrameAlignment);
  }
  set_register(SP, stack_pointer);

  // Prepare to execute the code at entry.
  set_register(PC, entry);
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  set_register(LR, kEndSimulatingPC);

  // Remember the values of callee-saved registers.
  // The code below assumes that r9 is not used as sb (static base) in
  // simulator code and therefore is regarded as a callee-saved register.
  int32_t r4_val = get_register(R4);
  int32_t r5_val = get_register(R5);
  int32_t r6_val = get_register(R6);
  int32_t r7_val = get_register(R7);
  int32_t r8_val = get_register(R8);
  int32_t r9_val = get_register(R9);
  int32_t r10_val = get_register(R10);
  int32_t r11_val = get_register(R11);

  // Setup the callee-saved registers with a known value. To be able to check
  // that they are preserved properly across dart execution.
  int32_t callee_saved_value = icount_;
  set_register(R4, callee_saved_value);
  set_register(R5, callee_saved_value);
  set_register(R6, callee_saved_value);
  set_register(R7, callee_saved_value);
  set_register(R8, callee_saved_value);
  set_register(R9, callee_saved_value);
  set_register(R10, callee_saved_value);
  set_register(R11, callee_saved_value);

  // Start the simulation
  Execute();

  // Check that the callee-saved registers have been preserved.
  ASSERT(callee_saved_value == get_register(R4));
  ASSERT(callee_saved_value == get_register(R5));
  ASSERT(callee_saved_value == get_register(R6));
  ASSERT(callee_saved_value == get_register(R7));
  ASSERT(callee_saved_value == get_register(R8));
  ASSERT(callee_saved_value == get_register(R9));
  ASSERT(callee_saved_value == get_register(R10));
  ASSERT(callee_saved_value == get_register(R11));

  // Restore callee-saved registers with the original value.
  set_register(R4, r4_val);
  set_register(R5, r5_val);
  set_register(R6, r6_val);
  set_register(R7, r7_val);
  set_register(R8, r8_val);
  set_register(R9, r9_val);
  set_register(R10, r10_val);
  set_register(R11, r11_val);

  // Restore the SP register and return R1:R0.
  set_register(SP, sp_before_call);
  return Utils::LowHighTo64Bits(get_register(R0), get_register(R1));
}


void Simulator::Longjmp(
    int32_t pc, int32_t sp, int32_t fp, const Instance& object) {
  set_register(SP, sp);
  set_register(FP, fp);
  set_register(PC, pc);
  SimulatorSetjmpBuffer* buf = last_setjmp_buffer();

  // Walk over all setjmp buffers (simulated --> C++ transitions)
  // and try to find the setjmp associated with the stack pointer.
  while (buf->link() != NULL && buf->link()->sp() <= sp) {
    buf = buf->link();
  }
  ASSERT(buf != NULL);

  // The caller has already cleaned up the stack memory of C++ frames.
  set_register(kExceptionObjectReg, bit_cast<int32_t>(object.raw()));
  buf->Longjmp();
}

}  // namespace dart

#endif  // !defined(HOST_ARCH_ARM)

#endif  // defined TARGET_ARCH_ARM
