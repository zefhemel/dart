// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"
#if defined(TARGET_ARCH_IA32)

#include "vm/assembler.h"
#include "vm/compiler.h"
#include "vm/dart_entry.h"
#include "vm/flow_graph_compiler.h"
#include "vm/instructions.h"
#include "vm/object_store.h"
#include "vm/pages.h"
#include "vm/resolver.h"
#include "vm/scavenger.h"
#include "vm/stub_code.h"


#define __ assembler->

namespace dart {

DEFINE_FLAG(bool, inline_alloc, true, "Inline allocation of objects.");
DEFINE_FLAG(bool, use_slow_path, false,
    "Set to true for debugging & verifying the slow paths.");
DECLARE_FLAG(int, optimization_counter_threshold);
DECLARE_FLAG(bool, trace_optimized_ic_calls);

// Input parameters:
//   ESP : points to return address.
//   ESP + 4 : address of last argument in argument array.
//   ESP + 4*EDX : address of first argument in argument array.
//   ESP + 4*EDX + 4 : address of return value.
//   ECX : address of the runtime function to call.
//   EDX : number of arguments to the call.
// Must preserve callee saved registers EDI and EBX.
void StubCode::GenerateCallToRuntimeStub(Assembler* assembler) {
  const intptr_t isolate_offset = NativeArguments::isolate_offset();
  const intptr_t argc_tag_offset = NativeArguments::argc_tag_offset();
  const intptr_t argv_offset = NativeArguments::argv_offset();
  const intptr_t retval_offset = NativeArguments::retval_offset();

  __ EnterFrame(0);

  // Load current Isolate pointer from Context structure into EAX.
  __ movl(EAX, FieldAddress(CTX, Context::isolate_offset()));

  // Save exit frame information to enable stack walking as we are about
  // to transition to Dart VM C++ code.
  __ movl(Address(EAX, Isolate::top_exit_frame_info_offset()), ESP);

  // Save current Context pointer into Isolate structure.
  __ movl(Address(EAX, Isolate::top_context_offset()), CTX);

  // Cache Isolate pointer into CTX while executing runtime code.
  __ movl(CTX, EAX);

  // Reserve space for arguments and align frame before entering C++ world.
  __ AddImmediate(ESP, Immediate(-sizeof(NativeArguments)));
  if (OS::ActivationFrameAlignment() > 0) {
    __ andl(ESP, Immediate(~(OS::ActivationFrameAlignment() - 1)));
  }

  // Pass NativeArguments structure by value and call runtime.
  __ movl(Address(ESP, isolate_offset), CTX);  // Set isolate in NativeArgs.
  // There are no runtime calls to closures, so we do not need to set the tag
  // bits kClosureFunctionBit and kInstanceFunctionBit in argc_tag_.
  __ movl(Address(ESP, argc_tag_offset), EDX);  // Set argc in NativeArguments.
  __ leal(EAX, Address(EBP, EDX, TIMES_4, 1 * kWordSize));  // Compute argv.
  __ movl(Address(ESP, argv_offset), EAX);  // Set argv in NativeArguments.
  __ addl(EAX, Immediate(1 * kWordSize));  // Retval is next to 1st argument.
  __ movl(Address(ESP, retval_offset), EAX);  // Set retval in NativeArguments.
  __ call(ECX);

  // Reset exit frame information in Isolate structure.
  __ movl(Address(CTX, Isolate::top_exit_frame_info_offset()), Immediate(0));

  // Load Context pointer from Isolate structure into ECX.
  __ movl(ECX, Address(CTX, Isolate::top_context_offset()));

  // Reset Context pointer in Isolate structure.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ movl(Address(CTX, Isolate::top_context_offset()), raw_null);

  // Cache Context pointer into CTX while executing Dart code.
  __ movl(CTX, ECX);

  __ LeaveFrame();
  __ ret();
}


// Print the stop message.
DEFINE_LEAF_RUNTIME_ENTRY(void, PrintStopMessage, const char* message) {
  OS::Print("Stop message: %s\n", message);
}
END_LEAF_RUNTIME_ENTRY


// Input parameters:
//   ESP : points to return address.
//   EAX : stop message (const char*).
// Must preserve all registers, except EAX.
void StubCode::GeneratePrintStopMessageStub(Assembler* assembler) {
  __ EnterCallRuntimeFrame(1 * kWordSize);
  __ movl(Address(ESP, 0), EAX);
  __ CallRuntime(kPrintStopMessageRuntimeEntry);
  __ LeaveCallRuntimeFrame();
  __ ret();
}


// Input parameters:
//   ESP : points to return address.
//   ESP + 4 : address of return value.
//   EAX : address of first argument in argument array.
//   ECX : address of the native function to call.
//   EDX : argc_tag including number of arguments and function kind.
// Uses EDI.
void StubCode::GenerateCallNativeCFunctionStub(Assembler* assembler) {
  const intptr_t native_args_struct_offset = kWordSize;
  const intptr_t isolate_offset =
      NativeArguments::isolate_offset() + native_args_struct_offset;
  const intptr_t argc_tag_offset =
      NativeArguments::argc_tag_offset() + native_args_struct_offset;
  const intptr_t argv_offset =
      NativeArguments::argv_offset() + native_args_struct_offset;
  const intptr_t retval_offset =
      NativeArguments::retval_offset() + native_args_struct_offset;

  __ EnterFrame(0);

  // Load current Isolate pointer from Context structure into EDI.
  __ movl(EDI, FieldAddress(CTX, Context::isolate_offset()));

  // Save exit frame information to enable stack walking as we are about
  // to transition to dart VM code.
  __ movl(Address(EDI, Isolate::top_exit_frame_info_offset()), ESP);

  // Save current Context pointer into Isolate structure.
  __ movl(Address(EDI, Isolate::top_context_offset()), CTX);

  // Cache Isolate pointer into CTX while executing native code.
  __ movl(CTX, EDI);

  // Reserve space for the native arguments structure, the outgoing parameter
  // (pointer to the native arguments structure) and align frame before
  // entering the C++ world.
  __ AddImmediate(ESP, Immediate(-sizeof(NativeArguments) - kWordSize));
  if (OS::ActivationFrameAlignment() > 0) {
    __ andl(ESP, Immediate(~(OS::ActivationFrameAlignment() - 1)));
  }

  // Pass NativeArguments structure by value and call native function.
  __ movl(Address(ESP, isolate_offset), CTX);  // Set isolate in NativeArgs.
  __ movl(Address(ESP, argc_tag_offset), EDX);  // Set argc in NativeArguments.
  __ movl(Address(ESP, argv_offset), EAX);  // Set argv in NativeArguments.
  __ leal(EAX, Address(EBP, 2 * kWordSize));  // Compute return value addr.
  __ movl(Address(ESP, retval_offset), EAX);  // Set retval in NativeArguments.
  __ leal(EAX, Address(ESP, kWordSize));  // Pointer to the NativeArguments.
  __ movl(Address(ESP, 0), EAX);  // Pass the pointer to the NativeArguments.
  __ call(ECX);

  // Reset exit frame information in Isolate structure.
  __ movl(Address(CTX, Isolate::top_exit_frame_info_offset()), Immediate(0));

  // Load Context pointer from Isolate structure into EDI.
  __ movl(EDI, Address(CTX, Isolate::top_context_offset()));

  // Reset Context pointer in Isolate structure.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ movl(Address(CTX, Isolate::top_context_offset()), raw_null);

  // Cache Context pointer into CTX while executing Dart code.
  __ movl(CTX, EDI);

  __ LeaveFrame();
  __ ret();
}


// Input parameters:
//   EDX: arguments descriptor array.
void StubCode::GenerateCallStaticFunctionStub(Assembler* assembler) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ EnterStubFrame();
  __ pushl(EDX);  // Preserve arguments descriptor array.
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ CallRuntime(kPatchStaticCallRuntimeEntry);
  __ popl(EAX);  // Get Code object result.
  __ popl(EDX);  // Restore arguments descriptor array.
  // Remove the stub frame as we are about to jump to the dart function.
  __ LeaveFrame();

  __ movl(ECX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(ECX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ jmp(ECX);
}


// Called from a static call only when an invalid code has been entered
// (invalid because its function was optimized or deoptimized).
// EDX: arguments descriptor array.
void StubCode::GenerateFixCallersTargetStub(Assembler* assembler) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(EDX);  // Preserve arguments descriptor array.
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ CallRuntime(kFixCallersTargetRuntimeEntry);
  __ popl(EAX);  // Get Code object.
  __ popl(EDX);  // Restore arguments descriptor array.
  __ movl(EAX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(EAX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ LeaveFrame();
  __ jmp(EAX);
  __ int3();
}


// Input parameters:
//   EDX: smi-tagged argument count, may be zero.
// Uses EAX, EBX, ECX, EDX.
static void PushArgumentsArray(Assembler* assembler, intptr_t arg_offset) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));

  // Allocate array to store arguments of caller.
  __ movl(ECX, raw_null);  // Null element type for raw Array.
  __ call(&StubCode::AllocateArrayLabel());
  __ SmiUntag(EDX);
  // EAX: newly allocated array.
  // EDX: length of the array (was preserved by the stub).
  __ pushl(EAX);  // Array is in EAX and on top of stack.
  __ leal(EBX, Address(ESP, EDX, TIMES_4, arg_offset));  // Addr of first arg.
  __ leal(ECX, FieldAddress(EAX, Array::data_offset()));
  Label loop, loop_condition;
  __ jmp(&loop_condition, Assembler::kNearJump);
  __ Bind(&loop);
  __ movl(EAX, Address(EBX, 0));
  __ movl(Address(ECX, 0), EAX);
  __ AddImmediate(ECX, Immediate(kWordSize));
  __ AddImmediate(EBX, Immediate(-kWordSize));
  __ Bind(&loop_condition);
  __ decl(EDX);
  __ j(POSITIVE, &loop, Assembler::kNearJump);
}


// Input parameters:
//   ECX: ic-data.
//   EDX: arguments descriptor array.
// Note: The receiver object is the first argument to the function being
//       called, the stub accesses the receiver from this location directly
//       when trying to resolve the call.
// Uses EDI.
void StubCode::GenerateInstanceFunctionLookupStub(Assembler* assembler) {
  __ EnterStubFrame();

  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ pushl(raw_null);  // Space for the return value.

  // Push the receiver as an argument.  Load the smi-tagged argument
  // count into EDI to index the receiver in the stack.  There are
  // three words (null, stub's pc marker, saved fp) above the return
  // address.
  __ movl(EDI, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  __ pushl(Address(ESP, EDI, TIMES_2, (3 * kWordSize)));

  __ pushl(ECX);  // Pass IC data object.
  __ pushl(EDX);  // Pass arguments descriptor array.

  // Pass the call's arguments array.
  __ movl(EDX, EDI);  // Smi-tagged arguments array length.
  PushArgumentsArray(assembler, (7 * kWordSize));
  // Stack layout explaining "(7 * kWordSize)" offset.
  // TOS + 0: Arguments array.
  // TOS + 1: Arguments descriptor array.
  // TOS + 2: IC data object.
  // TOS + 3: Receiver.
  // TOS + 4: Space for the result of the runtime call.
  // TOS + 5: Stub's PC marker (0)
  // TOS + 6: Saved FP
  // TOS + 7: Dart code return address
  // TOS + 8: Last argument of caller.
  // ....

  __ CallRuntime(kInstanceFunctionLookupRuntimeEntry);
  // Remove arguments.
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);  // Get result into EAX.
  __ LeaveFrame();
  __ ret();
}


DECLARE_LEAF_RUNTIME_ENTRY(intptr_t, DeoptimizeCopyFrame,
                           intptr_t deopt_reason,
                           uword saved_registers_address);

DECLARE_LEAF_RUNTIME_ENTRY(void, DeoptimizeFillFrame, uword last_fp);


// Used by eager and lazy deoptimization. Preserve result in EAX if necessary.
// This stub translates optimized frame into unoptimized frame. The optimized
// frame can contain values in registers and on stack, the unoptimized
// frame contains all values on stack.
// Deoptimization occurs in following steps:
// - Push all registers that can contain values.
// - Call C routine to copy the stack and saved registers into temporary buffer.
// - Adjust caller's frame to correct unoptimized frame size.
// - Fill the unoptimized frame.
// - Materialize objects that require allocation (e.g. Double instances).
// GC can occur only after frame is fully rewritten.
// Stack:
//   +------------------+
//   | Saved FP         | <- TOS
//   +------------------+
//   | return-address   |  (deoptimization point)
//   +------------------+
//   | optimized frame  |
//   |  ...             |
//
// Parts of the code cannot GC, part of the code can GC.
static void GenerateDeoptimizationSequence(Assembler* assembler,
                                           bool preserve_eax) {
  __ EnterFrame(0);
  // The code in this frame may not cause GC. kDeoptimizeCopyFrameRuntimeEntry
  // and kDeoptimizeFillFrameRuntimeEntry are leaf runtime calls.
  const intptr_t saved_eax_offset_from_ebp = -(kNumberOfCpuRegisters - EAX);
  // Result in EAX is preserved as part of pushing all registers below.

  // Push registers in their enumeration order: lowest register number at
  // lowest address.
  for (intptr_t i = kNumberOfCpuRegisters - 1; i >= 0; i--) {
    __ pushl(static_cast<Register>(i));
  }
  __ subl(ESP, Immediate(kNumberOfXmmRegisters * kFpuRegisterSize));
  intptr_t offset = 0;
  for (intptr_t reg_idx = 0; reg_idx < kNumberOfXmmRegisters; ++reg_idx) {
    XmmRegister xmm_reg = static_cast<XmmRegister>(reg_idx);
    __ movups(Address(ESP, offset), xmm_reg);
    offset += kFpuRegisterSize;
  }

  __ movl(ECX, ESP);  // Saved saved registers block.
  __ ReserveAlignedFrameSpace(1 * kWordSize);
  __ SmiUntag(EAX);
  __ movl(Address(ESP, 0), ECX);  // Start of register block.
  __ CallRuntime(kDeoptimizeCopyFrameRuntimeEntry);
  // Result (EAX) is stack-size (FP - SP) in bytes, incl. the return address.

  if (preserve_eax) {
    // Restore result into EBX temporarily.
    __ movl(EBX, Address(EBP, saved_eax_offset_from_ebp * kWordSize));
  }

  __ LeaveFrame();
  __ popl(EDX);  // Preserve return address.
  __ movl(ESP, EBP);
  __ subl(ESP, EAX);
  __ movl(Address(ESP, 0), EDX);

  __ EnterFrame(0);
  __ movl(ECX, ESP);  // Get last FP address.
  if (preserve_eax) {
    __ pushl(EBX);  // Preserve result.
  }
  __ ReserveAlignedFrameSpace(1 * kWordSize);
  __ movl(Address(ESP, 0), ECX);
  __ CallRuntime(kDeoptimizeFillFrameRuntimeEntry);
  // Result (EAX) is our FP.
  if (preserve_eax) {
    // Restore result into EBX.
    __ movl(EBX, Address(EBP, -1 * kWordSize));
  }
  // Code above cannot cause GC.
  __ LeaveFrame();
  __ movl(EBP, EAX);

  // Frame is fully rewritten at this point and it is safe to perform a GC.
  // Materialize any objects that were deferred by FillFrame because they
  // require allocation.
  __ EnterStubFrame();
  if (preserve_eax) {
    __ pushl(EBX);  // Preserve result, it will be GC-d here.
  }
  __ CallRuntime(kDeoptimizeMaterializeDoublesRuntimeEntry);
  if (preserve_eax) {
    __ popl(EAX);  // Restore result.
  }
  __ LeaveFrame();
  __ ret();
}


// TOS: return address + call-instruction-size (5 bytes).
// EAX: result, must be preserved
void StubCode::GenerateDeoptimizeLazyStub(Assembler* assembler) {
  // Correct return address to point just after the call that is being
  // deoptimized.
  __ popl(EBX);
  __ subl(EBX, Immediate(CallPattern::InstructionLength()));
  __ pushl(EBX);
  GenerateDeoptimizationSequence(assembler, true);  // Preserve EAX.
}


void StubCode::GenerateDeoptimizeStub(Assembler* assembler) {
  GenerateDeoptimizationSequence(assembler, false);  // Don't preserve EAX.
}


void StubCode::GenerateMegamorphicMissStub(Assembler* assembler) {
  __ EnterStubFrame();
  // Load the receiver into EAX.  The argument count in the arguments
  // descriptor in EDX is a smi.
  __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  // Two words (saved fp, stub's pc marker) in the stack above the return
  // address.
  __ movl(EAX, Address(ESP, EAX, TIMES_2, 2 * kWordSize));
  // Preserve IC data and arguments descriptor.
  __ pushl(ECX);
  __ pushl(EDX);

  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Instructions::null()));
  __ pushl(raw_null);  // Space for the result of the runtime call.
  __ pushl(EAX);  // Pass receiver.
  __ pushl(ECX);  // Pass IC data.
  __ pushl(EDX);  // Pass rguments descriptor.
  __ CallRuntime(kMegamorphicCacheMissHandlerRuntimeEntry);
  // Discard arguments.
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);  // Return value from the runtime call (instructions).
  __ popl(EDX);  // Restore arguments descriptor.
  __ popl(ECX);  // Restore IC data.
  __ LeaveFrame();

  Label lookup;
  __ cmpl(EAX, raw_null);
  __ j(EQUAL, &lookup, Assembler::kNearJump);
  __ addl(EAX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ jmp(EAX);

  __ Bind(&lookup);
  __ jmp(&StubCode::InstanceFunctionLookupLabel());
}


// Called for inline allocation of arrays.
// Input parameters:
//   EDX : Array length as Smi.
//   ECX : array element type (either NULL or an instantiated type).
// Uses EAX, EBX, ECX, EDI  as temporary registers.
// NOTE: EDX cannot be clobbered here as the caller relies on it being saved.
// The newly allocated object is returned in EAX.
void StubCode::GenerateAllocateArrayStub(Assembler* assembler) {
  Label slow_case;
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));

  if (FLAG_inline_alloc) {
    // Compute the size to be allocated, it is based on the array length
    // and is computed as:
    // RoundedAllocationSize((array_length * kwordSize) + sizeof(RawArray)).
    // Assert that length is a Smi.
    __ testl(EDX, Immediate(kSmiTagSize));
    if (FLAG_use_slow_path) {
      __ jmp(&slow_case);
    } else {
      __ j(NOT_ZERO, &slow_case);
    }
    __ movl(EDI, FieldAddress(CTX, Context::isolate_offset()));
    __ movl(EDI, Address(EDI, Isolate::heap_offset()));
    __ movl(EDI, Address(EDI, Heap::new_space_offset()));

    // Calculate and align allocation size.
    // Load new object start and calculate next object start.
    // ECX: array element type.
    // EDX: Array length as Smi.
    // EDI: Points to new space object.
    __ movl(EAX, Address(EDI, Scavenger::top_offset()));
    intptr_t fixed_size = sizeof(RawArray) + kObjectAlignment - 1;
    __ leal(EBX, Address(EDX, TIMES_2, fixed_size));  // EDX is Smi.
    ASSERT(kSmiTagShift == 1);
    __ andl(EBX, Immediate(-kObjectAlignment));
    __ leal(EBX, Address(EAX, EBX, TIMES_1, 0));

    // Check if the allocation fits into the remaining space.
    // EAX: potential new object start.
    // EBX: potential next object start.
    // ECX: array element type.
    // EDX: Array length as Smi.
    // EDI: Points to new space object.
    __ cmpl(EBX, Address(EDI, Scavenger::end_offset()));
    __ j(ABOVE_EQUAL, &slow_case);

    // Successfully allocated the object(s), now update top to point to
    // next object start and initialize the object.
    // EAX: potential new object start.
    // EBX: potential next object start.
    // EDX: Array length as Smi.
    // EDI: Points to new space object.
    __ movl(Address(EDI, Scavenger::top_offset()), EBX);
    __ addl(EAX, Immediate(kHeapObjectTag));

    // EAX: new object start as a tagged pointer.
    // EBX: new object end address.
    // ECX: array element type.
    // EDX: Array length as Smi.

    // Store the type argument field.
    __ StoreIntoObjectNoBarrier(
        EAX,
        FieldAddress(EAX, Array::type_arguments_offset()),
        ECX);

    // Set the length field.
    __ StoreIntoObjectNoBarrier(
        EAX,
        FieldAddress(EAX, Array::length_offset()),
        EDX);

    // Calculate the size tag.
    // EAX: new object start as a tagged pointer.
    // EBX: new object end address.
    // EDX: Array length as Smi.
    {
      Label size_tag_overflow, done;
      __ leal(ECX, Address(EDX, TIMES_2, fixed_size));  // EDX is Smi.
      ASSERT(kSmiTagShift == 1);
      __ andl(ECX, Immediate(-kObjectAlignment));
      __ cmpl(ECX, Immediate(RawObject::SizeTag::kMaxSizeTag));
      __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);
      __ shll(ECX, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));
      __ jmp(&done);

      __ Bind(&size_tag_overflow);
      __ movl(ECX, Immediate(0));
      __ Bind(&done);

      // Get the class index and insert it into the tags.
      __ orl(ECX, Immediate(RawObject::ClassIdTag::encode(kArrayCid)));
      __ movl(FieldAddress(EAX, Array::tags_offset()), ECX);
    }

    // Initialize all array elements to raw_null.
    // EAX: new object start as a tagged pointer.
    // EBX: new object end address.
    // EDX: Array length as Smi.
    __ leal(ECX, FieldAddress(EAX, Array::data_offset()));
    // ECX: iterator which initially points to the start of the variable
    // data area to be initialized.
    Label done;
    Label init_loop;
    __ Bind(&init_loop);
    __ cmpl(ECX, EBX);
    __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);
    // TODO(cshapiro): StoreIntoObjectNoBarrier
    __ movl(Address(ECX, 0), raw_null);
    __ addl(ECX, Immediate(kWordSize));
    __ jmp(&init_loop, Assembler::kNearJump);
    __ Bind(&done);

    // Done allocating and initializing the array.
    // EAX: new object.
    // EDX: Array length as Smi (preserved for the caller.)
    __ ret();
  }

  // Unable to allocate the array using the fast inline code, just call
  // into the runtime.
  __ Bind(&slow_case);
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ pushl(EDX);  // Array length as Smi.
  __ pushl(ECX);  // Element type.
  __ CallRuntime(kAllocateArrayRuntimeEntry);
  __ popl(EAX);  // Pop element type argument.
  __ popl(EDX);  // Pop array length argument.
  __ popl(EAX);  // Pop return value from return slot.
  __ LeaveFrame();
  __ ret();
}


// Input parameters:
//   EDX: Arguments descriptor array.
// Note: The closure object is the first argument to the function being
//       called, the stub accesses the closure from this location directly
//       when trying to resolve the call.
// Uses EDI.
void StubCode::GenerateCallClosureFunctionStub(Assembler* assembler) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));

  // Load num_args.
  __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  // Load closure object in EDI.
  __ movl(EDI, Address(ESP, EAX, TIMES_2, 0));  // EAX is a Smi.

  // Verify that EDI is a closure by checking its class.
  Label not_closure;
  __ cmpl(EDI, raw_null);
  // Not a closure, but null object.
  __ j(EQUAL, &not_closure, Assembler::kNearJump);
  __ testl(EDI, Immediate(kSmiTagMask));
  __ j(ZERO, &not_closure, Assembler::kNearJump);  // Not a closure, but a smi.
  // Verify that the class of the object is a closure class by checking that
  // class.signature_function() is not null.
  __ LoadClass(EAX, EDI, ECX);
  __ movl(EAX, FieldAddress(EAX, Class::signature_function_offset()));
  __ cmpl(EAX, raw_null);
  // Actual class is not a closure class.
  __ j(EQUAL, &not_closure, Assembler::kNearJump);

  // EAX is just the signature function. Load the actual closure function.
  __ movl(ECX, FieldAddress(EDI, Closure::function_offset()));

  // Load closure context in CTX; note that CTX has already been preserved.
  __ movl(CTX, FieldAddress(EDI, Closure::context_offset()));

  // Load closure function code in EAX.
  __ movl(EAX, FieldAddress(ECX, Function::code_offset()));
  __ cmpl(EAX, raw_null);
  Label function_compiled;
  __ j(NOT_EQUAL, &function_compiled, Assembler::kNearJump);

  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();

  __ pushl(EDX);  // Preserve arguments descriptor array.
  __ pushl(ECX);  // Preserve read-only function object argument.
  __ CallRuntime(kCompileFunctionRuntimeEntry);
  __ popl(ECX);  // Restore read-only function object argument in ECX.
  __ popl(EDX);  // Restore arguments descriptor array.
  // Restore EAX.
  __ movl(EAX, FieldAddress(ECX, Function::code_offset()));

  // Remove the stub frame as we are about to jump to the closure function.
  __ LeaveFrame();

  __ Bind(&function_compiled);
  // EAX: Code.
  // ECX: Function.
  // EDX: Arguments descriptor array.

  __ movl(ECX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(ECX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ jmp(ECX);

  __ Bind(&not_closure);
  // Call runtime to attempt to resolve and invoke a call method on a
  // non-closure object, passing the non-closure object and its arguments array,
  // returning here.
  // If no call method exists, throw a NoSuchMethodError.
  // EDI: non-closure object.
  // EDX: arguments descriptor array.

  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();

  __ pushl(raw_null);  // Setup space on stack for result from error reporting.
  __ pushl(EDX);  // Arguments descriptor.
  // Load smi-tagged arguments array length, including the non-closure.
  __ movl(EDX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  // See stack layout below explaining "wordSize * 5" offset.
  PushArgumentsArray(assembler, (kWordSize * 5));

  // Stack:
  // TOS + 0: Argument array.
  // TOS + 1: Arguments descriptor array.
  // TOS + 2: Place for result from the call.
  // TOS + 3: PC marker => RawInstruction object.
  // TOS + 4: Saved EBP of previous frame. <== EBP
  // TOS + 5: Dart code return address
  // TOS + 6: Last argument of caller.
  // ....
  __ CallRuntime(kInvokeNonClosureRuntimeEntry);
  // Remove arguments.
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);  // Get result into EAX.

  // Remove the stub frame as we are about to return.
  __ LeaveFrame();
  __ ret();
}


// Called when invoking dart code from C++ (VM code).
// Input parameters:
//   ESP : points to return address.
//   ESP + 4 : entrypoint of the dart function to call.
//   ESP + 8 : arguments descriptor array.
//   ESP + 12 : arguments array.
//   ESP + 16 : new context containing the current isolate pointer.
// Uses EAX, EDX, ECX, EDI as temporary registers.
void StubCode::GenerateInvokeDartCodeStub(Assembler* assembler) {
  const int kEntryPointOffset = 2 * kWordSize;
  const int kArgumentsDescOffset = 3 * kWordSize;
  const int kArgumentsOffset = 4 * kWordSize;
  const int kNewContextOffset = 5 * kWordSize;

  // Save frame pointer coming in.
  __ EnterFrame(0);

  // Save C++ ABI callee-saved registers.
  __ pushl(EBX);
  __ pushl(ESI);
  __ pushl(EDI);

  // The new Context structure contains a pointer to the current Isolate
  // structure. Cache the Context pointer in the CTX register so that it is
  // available in generated code and calls to Isolate::Current() need not be
  // done. The assumption is that this register will never be clobbered by
  // compiled or runtime stub code.

  // Cache the new Context pointer into CTX while executing dart code.
  __ movl(CTX, Address(EBP, kNewContextOffset));
  __ movl(CTX, Address(CTX, VMHandles::kOffsetOfRawPtrInHandle));

  // Load Isolate pointer from Context structure into EDI.
  __ movl(EDI, FieldAddress(CTX, Context::isolate_offset()));

  // Save the top exit frame info. Use EDX as a temporary register.
  // StackFrameIterator reads the top exit frame info saved in this frame.
  // The constant kExitLinkOffsetInEntryFrame must be kept in sync with the
  // code below: kExitLinkOffsetInEntryFrame = -4 * kWordSize.
  __ movl(EDX, Address(EDI, Isolate::top_exit_frame_info_offset()));
  __ pushl(EDX);
  __ movl(Address(EDI, Isolate::top_exit_frame_info_offset()), Immediate(0));

  // Save the old Context pointer. Use ECX as a temporary register.
  // Note that VisitObjectPointers will find this saved Context pointer during
  // GC marking, since it traverses any information between SP and
  // FP - kExitLinkOffsetInEntryFrame.
  // EntryFrame::SavedContext reads the context saved in this frame.
  // The constant kSavedContextOffsetInEntryFrame must be kept in sync with
  // the code below: kSavedContextOffsetInEntryFrame = -5 * kWordSize.
  __ movl(ECX, Address(EDI, Isolate::top_context_offset()));
  __ pushl(ECX);

  // Load arguments descriptor array into EDX.
  __ movl(EDX, Address(EBP, kArgumentsDescOffset));
  __ movl(EDX, Address(EDX, VMHandles::kOffsetOfRawPtrInHandle));

  // Load number of arguments into EBX.
  __ movl(EBX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  __ SmiUntag(EBX);

  // Set up arguments for the dart call.
  Label push_arguments;
  Label done_push_arguments;
  __ testl(EBX, EBX);  // check if there are arguments.
  __ j(ZERO, &done_push_arguments, Assembler::kNearJump);
  __ movl(EAX, Immediate(0));

  // Compute address of 'arguments array' data area into EDI.
  __ movl(EDI, Address(EBP, kArgumentsOffset));
  __ movl(EDI, Address(EDI, VMHandles::kOffsetOfRawPtrInHandle));
  __ leal(EDI, FieldAddress(EDI, Array::data_offset()));

  __ Bind(&push_arguments);
  __ movl(ECX, Address(EDI, EAX, TIMES_4, 0));
  __ pushl(ECX);
  __ incl(EAX);
  __ cmpl(EAX, EBX);
  __ j(LESS, &push_arguments, Assembler::kNearJump);
  __ Bind(&done_push_arguments);

  // Call the dart code entrypoint.
  __ call(Address(EBP, kEntryPointOffset));

  // Reread the Context pointer.
  __ movl(CTX, Address(EBP, kNewContextOffset));
  __ movl(CTX, Address(CTX, VMHandles::kOffsetOfRawPtrInHandle));

  // Reread the arguments descriptor array to obtain the number of passed
  // arguments.
  __ movl(EDX, Address(EBP, kArgumentsDescOffset));
  __ movl(EDX, Address(EDX, VMHandles::kOffsetOfRawPtrInHandle));
  __ movl(EDX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  // Get rid of arguments pushed on the stack.
  __ leal(ESP, Address(ESP, EDX, TIMES_2, 0));  // EDX is a Smi.

  // Load Isolate pointer from Context structure into CTX. Drop Context.
  __ movl(CTX, FieldAddress(CTX, Context::isolate_offset()));

  // Restore the saved Context pointer into the Isolate structure.
  // Uses ECX as a temporary register for this.
  __ popl(ECX);
  __ movl(Address(CTX, Isolate::top_context_offset()), ECX);

  // Restore the saved top exit frame info back into the Isolate structure.
  // Uses EDX as a temporary register for this.
  __ popl(EDX);
  __ movl(Address(CTX, Isolate::top_exit_frame_info_offset()), EDX);

  // Restore C++ ABI callee-saved registers.
  __ popl(EDI);
  __ popl(ESI);
  __ popl(EBX);

  // Restore the frame pointer.
  __ LeaveFrame();

  __ ret();
}


// Called for inline allocation of contexts.
// Input:
// EDX: number of context variables.
// Output:
// EAX: new allocated RawContext object.
// EBX and EDX are destroyed.
void StubCode::GenerateAllocateContextStub(Assembler* assembler) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  if (FLAG_inline_alloc) {
    const Class& context_class = Class::ZoneHandle(Object::context_class());
    Label slow_case;
    Heap* heap = Isolate::Current()->heap();
    // First compute the rounded instance size.
    // EDX: number of context variables.
    intptr_t fixed_size = (sizeof(RawContext) + kObjectAlignment - 1);
    __ leal(EBX, Address(EDX, TIMES_4, fixed_size));
    __ andl(EBX, Immediate(-kObjectAlignment));

    // Now allocate the object.
    // EDX: number of context variables.
    __ movl(EAX, Address::Absolute(heap->TopAddress()));
    __ addl(EBX, EAX);
    // Check if the allocation fits into the remaining space.
    // EAX: potential new object.
    // EBX: potential next object start.
    // EDX: number of context variables.
    __ cmpl(EBX, Address::Absolute(heap->EndAddress()));
    if (FLAG_use_slow_path) {
      __ jmp(&slow_case);
    } else {
      __ j(ABOVE_EQUAL, &slow_case, Assembler::kNearJump);
    }

    // Successfully allocated the object, now update top to point to
    // next object start and initialize the object.
    // EAX: new object.
    // EBX: next object start.
    // EDX: number of context variables.
    __ movl(Address::Absolute(heap->TopAddress()), EBX);
    __ addl(EAX, Immediate(kHeapObjectTag));

    // Calculate the size tag.
    // EAX: new object.
    // EDX: number of context variables.
    {
      Label size_tag_overflow, done;
      __ leal(EBX, Address(EDX, TIMES_4, fixed_size));
      __ andl(EBX, Immediate(-kObjectAlignment));
      __ cmpl(EBX, Immediate(RawObject::SizeTag::kMaxSizeTag));
      __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);
      __ shll(EBX, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));
      __ jmp(&done);

      __ Bind(&size_tag_overflow);
      // Set overflow size tag value.
      __ movl(EBX, Immediate(0));

      __ Bind(&done);
      // EAX: new object.
      // EDX: number of context variables.
      // EBX: size and bit tags.
      __ orl(EBX,
             Immediate(RawObject::ClassIdTag::encode(context_class.id())));
      __ movl(FieldAddress(EAX, Context::tags_offset()), EBX);  // Tags.
    }

    // Setup up number of context variables field.
    // EAX: new object.
    // EDX: number of context variables as integer value (not object).
    __ movl(FieldAddress(EAX, Context::num_variables_offset()), EDX);

    // Setup isolate field.
    // Load Isolate pointer from Context structure into EBX.
    // EAX: new object.
    // EDX: number of context variables.
    __ movl(EBX, FieldAddress(CTX, Context::isolate_offset()));
    // EBX: Isolate, not an object.
    __ movl(FieldAddress(EAX, Context::isolate_offset()), EBX);

    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    // Setup the parent field.
    // EAX: new object.
    // EDX: number of context variables.
    __ movl(FieldAddress(EAX, Context::parent_offset()), raw_null);

    // Initialize the context variables.
    // EAX: new object.
    // EDX: number of context variables.
    {
      Label loop, entry;
      __ leal(EBX, FieldAddress(EAX, Context::variable_offset(0)));

      __ jmp(&entry, Assembler::kNearJump);
      __ Bind(&loop);
      __ decl(EDX);
      __ movl(Address(EBX, EDX, TIMES_4, 0), raw_null);
      __ Bind(&entry);
      __ cmpl(EDX, Immediate(0));
      __ j(NOT_EQUAL, &loop, Assembler::kNearJump);
    }

    // Done allocating and initializing the context.
    // EAX: new object.
    __ ret();

    __ Bind(&slow_case);
  }
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ SmiTag(EDX);
  __ pushl(EDX);
  __ CallRuntime(kAllocateContextRuntimeEntry);  // Allocate context.
  __ popl(EAX);  // Pop number of context variables argument.
  __ popl(EAX);  // Pop the new context object.
  // EAX: new object
  // Restore the frame pointer.
  __ LeaveFrame();
  __ ret();
}

DECLARE_LEAF_RUNTIME_ENTRY(void, StoreBufferBlockProcess, Isolate* isolate);

// Helper stub to implement Assembler::StoreIntoObject.
// Input parameters:
//   EAX: Address being stored
void StubCode::GenerateUpdateStoreBufferStub(Assembler* assembler) {
  // Save values being destroyed.
  __ pushl(EDX);
  __ pushl(ECX);

  // Load the isolate out of the context.
  // Spilled: EDX, ECX
  // EAX: Address being stored
  __ movl(EDX, FieldAddress(CTX, Context::isolate_offset()));

  // Load top_ out of the StoreBufferBlock and add the address to the pointers_.
  // Spilled: EDX, ECX
  // EAX: Address being stored
  // EDX: Isolate
  intptr_t store_buffer_offset = Isolate::store_buffer_block_offset();
  __ movl(ECX,
          Address(EDX, store_buffer_offset + StoreBufferBlock::top_offset()));
  __ movl(Address(EDX,
                  ECX, TIMES_4,
                  store_buffer_offset + StoreBufferBlock::pointers_offset()),
          EAX);

  // Increment top_ and check for overflow.
  // Spilled: EDX, ECX
  // ECX: top_
  // EDX: Isolate
  Label L;
  __ incl(ECX);
  __ movl(Address(EDX, store_buffer_offset + StoreBufferBlock::top_offset()),
          ECX);
  __ cmpl(ECX, Immediate(StoreBufferBlock::kSize));
  // Restore values.
  // Spilled: EDX, ECX
  __ popl(ECX);
  __ popl(EDX);
  __ j(EQUAL, &L, Assembler::kNearJump);
  __ ret();

  // Handle overflow: Call the runtime leaf function.
  __ Bind(&L);
  // Setup frame, push callee-saved registers.

  __ EnterCallRuntimeFrame(1 * kWordSize);
  __ movl(EAX, FieldAddress(CTX, Context::isolate_offset()));
  __ movl(Address(ESP, 0), EAX);  // Push the isolate as the only argument.
  __ CallRuntime(kStoreBufferBlockProcessRuntimeEntry);
  // Restore callee-saved registers, tear down frame.
  __ LeaveCallRuntimeFrame();
  __ ret();
}


// Called for inline allocation of objects.
// Input parameters:
//   ESP + 8 : type arguments object (only if class is parameterized).
//   ESP + 4 : type arguments of instantiator (only if class is parameterized).
//   ESP : points to return address.
// Uses EAX, EBX, ECX, EDX, EDI as temporary registers.
void StubCode::GenerateAllocationStubForClass(Assembler* assembler,
                                              const Class& cls) {
  const intptr_t kObjectTypeArgumentsOffset = 2 * kWordSize;
  const intptr_t kInstantiatorTypeArgumentsOffset = 1 * kWordSize;
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  // The generated code is different if the class is parameterized.
  const bool is_cls_parameterized =
      cls.type_arguments_field_offset() != Class::kNoTypeArguments;
  // kInlineInstanceSize is a constant used as a threshold for determining
  // when the object initialization should be done as a loop or as
  // straight line code.
  const int kInlineInstanceSize = 12;
  const intptr_t instance_size = cls.instance_size();
  ASSERT(instance_size > 0);
  const intptr_t type_args_size = InstantiatedTypeArguments::InstanceSize();
  if (FLAG_inline_alloc &&
      PageSpace::IsPageAllocatableSize(instance_size + type_args_size)) {
    Label slow_case;
    Heap* heap = Isolate::Current()->heap();
    __ movl(EAX, Address::Absolute(heap->TopAddress()));
    __ leal(EBX, Address(EAX, instance_size));
    if (is_cls_parameterized) {
      __ movl(ECX, EBX);
      // A new InstantiatedTypeArguments object only needs to be allocated if
      // the instantiator is provided (not kNoInstantiator, but may be null).
      Label no_instantiator;
      __ cmpl(Address(ESP, kInstantiatorTypeArgumentsOffset),
              Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
      __ j(EQUAL, &no_instantiator, Assembler::kNearJump);
      __ addl(EBX, Immediate(type_args_size));
      __ Bind(&no_instantiator);
      // ECX: potential new object end and, if ECX != EBX, potential new
      // InstantiatedTypeArguments object start.
    }
    // Check if the allocation fits into the remaining space.
    // EAX: potential new object start.
    // EBX: potential next object start.
    __ cmpl(EBX, Address::Absolute(heap->EndAddress()));
    if (FLAG_use_slow_path) {
      __ jmp(&slow_case);
    } else {
      __ j(ABOVE_EQUAL, &slow_case, Assembler::kNearJump);
    }

    // Successfully allocated the object(s), now update top to point to
    // next object start and initialize the object.
    __ movl(Address::Absolute(heap->TopAddress()), EBX);

    if (is_cls_parameterized) {
      // Initialize the type arguments field in the object.
      // EAX: new object start.
      // ECX: potential new object end and, if ECX != EBX, potential new
      // InstantiatedTypeArguments object start.
      // EBX: next object start.
      Label type_arguments_ready;
      __ movl(EDI, Address(ESP, kObjectTypeArgumentsOffset));
      __ cmpl(ECX, EBX);
      __ j(EQUAL, &type_arguments_ready, Assembler::kNearJump);
      // Initialize InstantiatedTypeArguments object at ECX.
      __ movl(Address(ECX,
          InstantiatedTypeArguments::uninstantiated_type_arguments_offset()),
              EDI);
      __ movl(EDX, Address(ESP, kInstantiatorTypeArgumentsOffset));
      __ movl(Address(ECX,
          InstantiatedTypeArguments::instantiator_type_arguments_offset()),
              EDX);
      const Class& ita_cls =
          Class::ZoneHandle(Object::instantiated_type_arguments_class());
      // Set the tags.
      uword tags = 0;
      tags = RawObject::SizeTag::update(type_args_size, tags);
      tags = RawObject::ClassIdTag::update(ita_cls.id(), tags);
      __ movl(Address(ECX, Instance::tags_offset()), Immediate(tags));
      // Set the new InstantiatedTypeArguments object (ECX) as the type
      // arguments (EDI) of the new object (EAX).
      __ movl(EDI, ECX);
      __ addl(EDI, Immediate(kHeapObjectTag));
      // Set EBX to new object end.
      __ movl(EBX, ECX);
      __ Bind(&type_arguments_ready);
      // EAX: new object.
      // EDI: new object type arguments.
    }

    // EAX: new object start.
    // EBX: next object start.
    // EDI: new object type arguments (if is_cls_parameterized).
    // Set the tags.
    uword tags = 0;
    tags = RawObject::SizeTag::update(instance_size, tags);
    ASSERT(cls.id() != kIllegalCid);
    tags = RawObject::ClassIdTag::update(cls.id(), tags);
    __ movl(Address(EAX, Instance::tags_offset()), Immediate(tags));

    // Initialize the remaining words of the object.
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));

    // EAX: new object start.
    // EBX: next object start.
    // EDI: new object type arguments (if is_cls_parameterized).
    // First try inlining the initialization without a loop.
    if (instance_size < (kInlineInstanceSize * kWordSize)) {
      // Check if the object contains any non-header fields.
      // Small objects are initialized using a consecutive set of writes.
      for (intptr_t current_offset = sizeof(RawObject);
           current_offset < instance_size;
           current_offset += kWordSize) {
        __ movl(Address(EAX, current_offset), raw_null);
      }
    } else {
      __ leal(ECX, Address(EAX, sizeof(RawObject)));
      // Loop until the whole object is initialized.
      // EAX: new object.
      // EBX: next object start.
      // ECX: next word to be initialized.
      // EDI: new object type arguments (if is_cls_parameterized).
      Label init_loop;
      Label done;
      __ Bind(&init_loop);
      __ cmpl(ECX, EBX);
      __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);
      __ movl(Address(ECX, 0), raw_null);
      __ addl(ECX, Immediate(kWordSize));
      __ jmp(&init_loop, Assembler::kNearJump);
      __ Bind(&done);
    }
    if (is_cls_parameterized) {
      // EDI: new object type arguments.
      // Set the type arguments in the new object.
      __ movl(Address(EAX, cls.type_arguments_field_offset()), EDI);
    }
    // Done allocating and initializing the instance.
    // EAX: new object.
    __ addl(EAX, Immediate(kHeapObjectTag));
    __ ret();

    __ Bind(&slow_case);
  }
  if (is_cls_parameterized) {
    __ movl(EAX, Address(ESP, kObjectTypeArgumentsOffset));
    __ movl(EDX, Address(ESP, kInstantiatorTypeArgumentsOffset));
  }
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ PushObject(cls);  // Push class of object to be allocated.
  if (is_cls_parameterized) {
    __ pushl(EAX);  // Push type arguments of object to be allocated.
    __ pushl(EDX);  // Push type arguments of instantiator.
  } else {
    __ pushl(raw_null);  // Push null type arguments.
    __ pushl(Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
  }
  __ CallRuntime(kAllocateObjectRuntimeEntry);  // Allocate object.
  __ popl(EAX);  // Pop argument (instantiator).
  __ popl(EAX);  // Pop argument (type arguments of object).
  __ popl(EAX);  // Pop argument (class of object).
  __ popl(EAX);  // Pop result (newly allocated object).
  // EAX: new object
  // Restore the frame pointer.
  __ LeaveFrame();
  __ ret();
}


// Called for inline allocation of closures.
// Input parameters:
//   ESP + 8 : receiver (null if not an implicit instance closure).
//   ESP + 4 : type arguments object (null if class is no parameterized).
//   ESP : points to return address.
// Uses EAX, EBX, ECX, EDX as temporary registers.
void StubCode::GenerateAllocationStubForClosure(Assembler* assembler,
                                                const Function& func) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  ASSERT(func.IsClosureFunction());
  const bool is_implicit_static_closure =
      func.IsImplicitStaticClosureFunction();
  const bool is_implicit_instance_closure =
      func.IsImplicitInstanceClosureFunction();
  const Class& cls = Class::ZoneHandle(func.signature_class());
  const bool has_type_arguments = cls.HasTypeArguments();
  const intptr_t kTypeArgumentsOffset = 1 * kWordSize;
  const intptr_t kReceiverOffset = 2 * kWordSize;
  const intptr_t closure_size = Closure::InstanceSize();
  const intptr_t context_size = Context::InstanceSize(1);  // Captured receiver.
  if (FLAG_inline_alloc &&
      PageSpace::IsPageAllocatableSize(closure_size + context_size)) {
    Label slow_case;
    Heap* heap = Isolate::Current()->heap();
    __ movl(EAX, Address::Absolute(heap->TopAddress()));
    __ leal(EBX, Address(EAX, closure_size));
    if (is_implicit_instance_closure) {
      __ movl(ECX, EBX);  // ECX: new context address.
      __ addl(EBX, Immediate(context_size));
    }
    // Check if the allocation fits into the remaining space.
    // EAX: potential new closure object.
    // ECX: potential new context object (only if is_implicit_closure).
    // EBX: potential next object start.
    __ cmpl(EBX, Address::Absolute(heap->EndAddress()));
    if (FLAG_use_slow_path) {
      __ jmp(&slow_case);
    } else {
      __ j(ABOVE_EQUAL, &slow_case, Assembler::kNearJump);
    }

    // Successfully allocated the object, now update top to point to
    // next object start and initialize the object.
    __ movl(Address::Absolute(heap->TopAddress()), EBX);

    // EAX: new closure object.
    // ECX: new context object (only if is_implicit_closure).
    // Set the tags.
    uword tags = 0;
    tags = RawObject::SizeTag::update(closure_size, tags);
    tags = RawObject::ClassIdTag::update(cls.id(), tags);
    __ movl(Address(EAX, Instance::tags_offset()), Immediate(tags));

    // Initialize the function field in the object.
    // EAX: new closure object.
    // ECX: new context object (only if is_implicit_closure).
    // EBX: next object start.
    __ LoadObject(EDX, func);  // Load function of closure to be allocated.
    __ movl(Address(EAX, Closure::function_offset()), EDX);

    // Setup the context for this closure.
    if (is_implicit_static_closure) {
      ObjectStore* object_store = Isolate::Current()->object_store();
      ASSERT(object_store != NULL);
      const Context& empty_context =
          Context::ZoneHandle(object_store->empty_context());
      __ LoadObject(EDX, empty_context);
      __ movl(Address(EAX, Closure::context_offset()), EDX);
    } else if (is_implicit_instance_closure) {
      // Initialize the new context capturing the receiver.
      const Class& context_class = Class::ZoneHandle(Object::context_class());
      // Set the tags.
      uword tags = 0;
      tags = RawObject::SizeTag::update(context_size, tags);
      tags = RawObject::ClassIdTag::update(context_class.id(), tags);
      __ movl(Address(ECX, Context::tags_offset()), Immediate(tags));

      // Set number of variables field to 1 (for captured receiver).
      __ movl(Address(ECX, Context::num_variables_offset()), Immediate(1));

      // Set isolate field to isolate of current context.
      __ movl(EDX, FieldAddress(CTX, Context::isolate_offset()));
      __ movl(Address(ECX, Context::isolate_offset()), EDX);

      // Set the parent to null.
      __ movl(Address(ECX, Context::parent_offset()), raw_null);

      // Initialize the context variable to the receiver.
      __ movl(EDX, Address(ESP, kReceiverOffset));
      __ movl(Address(ECX, Context::variable_offset(0)), EDX);

      // Set the newly allocated context in the newly allocated closure.
      __ addl(ECX, Immediate(kHeapObjectTag));
      __ movl(Address(EAX, Closure::context_offset()), ECX);
    } else {
      __ movl(Address(EAX, Closure::context_offset()), CTX);
    }

    // Set the type arguments field in the newly allocated closure.
    __ movl(EDX, Address(ESP, kTypeArgumentsOffset));
    __ movl(Address(EAX, Closure::type_arguments_offset()), EDX);

    // Done allocating and initializing the instance.
    // EAX: new object.
    __ addl(EAX, Immediate(kHeapObjectTag));
    __ ret();

    __ Bind(&slow_case);
  }
  if (has_type_arguments) {
    __ movl(ECX, Address(ESP, kTypeArgumentsOffset));
  }
  if (is_implicit_instance_closure) {
    __ movl(EAX, Address(ESP, kReceiverOffset));
  }
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ PushObject(func);
  if (is_implicit_static_closure) {
    __ CallRuntime(kAllocateImplicitStaticClosureRuntimeEntry);
  } else {
    if (is_implicit_instance_closure) {
      __ pushl(EAX);  // Receiver.
    }
    if (has_type_arguments) {
      __ pushl(ECX);  // Push type arguments of closure to be allocated.
    } else {
      __ pushl(raw_null);  // Push null type arguments.
    }
    if (is_implicit_instance_closure) {
      __ CallRuntime(kAllocateImplicitInstanceClosureRuntimeEntry);
      __ popl(EAX);  // Pop argument (type arguments of object).
      __ popl(EAX);  // Pop receiver.
    } else {
      ASSERT(func.IsNonImplicitClosureFunction());
      __ CallRuntime(kAllocateClosureRuntimeEntry);
      __ popl(EAX);  // Pop argument (type arguments of object).
    }
  }
  __ popl(EAX);  // Pop function object.
  __ popl(EAX);
  // EAX: new object
  // Restore the frame pointer.
  __ LeaveFrame();
  __ ret();
}


// Called for invoking noSuchMethod function from the entry code of a dart
// function after an error in passed named arguments is detected.
// Input parameters:
//   EBP - 4 : PC marker => RawInstruction object.
//   EBP : points to previous frame pointer.
//   EBP + 4 : points to return address.
//   EBP + 8 : address of last argument (arg n-1).
//   EBP + 8 + 4*(n-1) : address of first argument (arg 0).
//   ECX : ic-data.
//   EDX : arguments descriptor array.
// Uses EAX, EBX, EDI as temporary registers.
void StubCode::GenerateCallNoSuchMethodFunctionStub(Assembler* assembler) {
  // The target function was not found, so invoke method
  // "dynamic noSuchMethod(Invocation invocation)".
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ movl(EDI, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  __ movl(EAX, Address(EBP, EDI, TIMES_2, kWordSize));  // Get receiver.

  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();

  __ pushl(raw_null);  // Setup space on stack for result from noSuchMethod.
  __ pushl(EAX);  // Receiver.
  __ pushl(ECX);  // IC data array.
  __ pushl(EDX);  // Arguments descriptor array.

  __ movl(EDX, EDI);
  // See stack layout below explaining "wordSize * 10" offset.
  PushArgumentsArray(assembler, (kWordSize * 10));

  // Stack:
  // TOS + 0: Argument array.
  // TOS + 1: Arguments descriptor array.
  // TOS + 2: Ic-data.
  // TOS + 3: Receiver.
  // TOS + 4: Place for result from noSuchMethod.
  // TOS + 5: PC marker => RawInstruction object.
  // TOS + 6: Saved EBP of previous frame. <== EBP
  // TOS + 7: Dart callee (or stub) code return address
  // TOS + 8: PC marker => RawInstruction object of dart caller frame.
  // TOS + 9: Saved EBP of dart caller frame.
  // TOS + 10: Dart caller code return address
  // TOS + 11: Last argument of caller.
  // ....
  __ CallRuntime(kInvokeNoSuchMethodFunctionRuntimeEntry);
  // Remove arguments.
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);
  __ popl(EAX);  // Get result into EAX.

  // Remove the stub frame as we are about to return.
  __ LeaveFrame();
  __ ret();
}


void StubCode::GenerateOptimizedUsageCounterIncrement(Assembler* assembler) {
  Register argdesc_reg = EDX;
  Register ic_reg = ECX;
  Register func_reg = EDI;
  if (FLAG_trace_optimized_ic_calls) {
    __ EnterStubFrame();
    __ pushl(func_reg);     // Preserve
    __ pushl(argdesc_reg);  // Preserve.
    __ pushl(ic_reg);       // Preserve.
    __ pushl(ic_reg);       // Argument.
    __ pushl(func_reg);     // Argument.
    __ CallRuntime(kTraceICCallRuntimeEntry);
    __ popl(EAX);          // Discard argument;
    __ popl(EAX);          // Discard argument;
    __ popl(ic_reg);       // Restore.
    __ popl(argdesc_reg);  // Restore.
    __ popl(func_reg);     // Restore.
    __ LeaveFrame();
  }
  Label is_hot;
  if (FlowGraphCompiler::CanOptimize()) {
    ASSERT(FLAG_optimization_counter_threshold > 1);
    __ cmpl(FieldAddress(func_reg, Function::usage_counter_offset()),
        Immediate(FLAG_optimization_counter_threshold));
    __ j(GREATER_EQUAL, &is_hot, Assembler::kNearJump);
    // As long as VM has no OSR do not optimize in the middle of the function
    // but only at exit so that we have collected all type feedback before
    // optimizing.
  }
  __ incl(FieldAddress(func_reg, Function::usage_counter_offset()));
  __ Bind(&is_hot);
}



// Loads function into 'temp_reg'.
void StubCode::GenerateUsageCounterIncrement(Assembler* assembler,
                                             Register temp_reg) {
  Register ic_reg = ECX;
  Register func_reg = temp_reg;
  ASSERT(ic_reg != func_reg);
  __ movl(func_reg, FieldAddress(ic_reg, ICData::function_offset()));
  Label is_hot;
  if (FlowGraphCompiler::CanOptimize()) {
    ASSERT(FLAG_optimization_counter_threshold > 1);
    // The usage_counter is always less than FLAG_optimization_counter_threshold
    // except when the function gets optimized.
    __ cmpl(FieldAddress(func_reg, Function::usage_counter_offset()),
        Immediate(FLAG_optimization_counter_threshold));
    __ j(EQUAL, &is_hot, Assembler::kNearJump);
    // As long as VM has no OSR do not optimize in the middle of the function
    // but only at exit so that we have collected all type feedback before
    // optimizing.
  }
  __ incl(FieldAddress(func_reg, Function::usage_counter_offset()));
  __ Bind(&is_hot);
}


// Generate inline cache check for 'num_args'.
//  ECX: Inline cache data object.
//  EDX: Arguments descriptor array.
//  TOS(0): return address
// Control flow:
// - If receiver is null -> jump to IC miss.
// - If receiver is Smi -> load Smi class.
// - If receiver is not-Smi -> load receiver's class.
// - Check if 'num_args' (including receiver) match any IC data group.
// - Match found -> jump to target.
// - Match not found -> jump to IC miss.
void StubCode::GenerateNArgsCheckInlineCacheStub(Assembler* assembler,
                                                 intptr_t num_args) {
  ASSERT(num_args > 0);
#if defined(DEBUG)
  { Label ok;
    // Check that the IC data array has NumberOfArgumentsChecked() == num_args.
    // 'num_args_tested' is stored as an untagged int.
    __ movl(EBX, FieldAddress(ECX, ICData::num_args_tested_offset()));
    __ cmpl(EBX, Immediate(num_args));
    __ j(EQUAL, &ok, Assembler::kNearJump);
    __ Stop("Incorrect stub for IC data");
    __ Bind(&ok);
  }
#endif  // DEBUG

  // Loop that checks if there is an IC data match.
  Label loop, update, test, found, get_class_id_as_smi;
  // ECX: IC data object (preserved).
  __ movl(EBX, FieldAddress(ECX, ICData::ic_data_offset()));
  // EBX: ic_data_array with check entries: classes and target functions.
  __ leal(EBX, FieldAddress(EBX, Array::data_offset()));
  // EBX: points directly to the first ic data array element.

  // Get the receiver's class ID (first read number of arguments from
  // arguments descriptor array and then access the receiver from the stack).
  __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  __ movl(EAX, Address(ESP, EAX, TIMES_2, 0));  // EAX (argument_count) is smi.
  __ call(&get_class_id_as_smi);
  // EAX: receiver's class ID (smi).
  __ movl(EDI, Address(EBX, 0));  // First class id (smi) to check.
  __ jmp(&test);

  __ Bind(&loop);
  for (int i = 0; i < num_args; i++) {
    if (i > 0) {
      // If not the first, load the next argument's class ID.
      __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
      __ movl(EAX, Address(ESP, EAX, TIMES_2, - i * kWordSize));
      __ call(&get_class_id_as_smi);
      // EAX: next argument class ID (smi).
      __ movl(EDI, Address(EBX, i * kWordSize));
      // EDI: next class ID to check (smi).
    }
    __ cmpl(EAX, EDI);  // Class id match?
    if (i < (num_args - 1)) {
      __ j(NOT_EQUAL, &update);  // Continue.
    } else {
      // Last check, all checks before matched.
      __ j(EQUAL, &found, Assembler::kNearJump);  // Break.
    }
  }
  __ Bind(&update);
  // Reload receiver class ID.  It has not been destroyed when num_args == 1.
  if (num_args > 1) {
    __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
    __ movl(EAX, Address(ESP, EAX, TIMES_2, 0));
    __ call(&get_class_id_as_smi);
  }

  const intptr_t entry_size = ICData::TestEntryLengthFor(num_args) * kWordSize;
  __ addl(EBX, Immediate(entry_size));  // Next entry.
  __ movl(EDI, Address(EBX, 0));  // Next class ID.

  __ Bind(&test);
  __ cmpl(EDI, Immediate(Smi::RawValue(kIllegalCid)));  // Done?
  __ j(NOT_EQUAL, &loop, Assembler::kNearJump);

  // IC miss.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  // Compute address of arguments (first read number of arguments from
  // arguments descriptor array and then compute address on the stack).
  __ movl(EAX, FieldAddress(EDX, ArgumentsDescriptor::count_offset()));
  __ leal(EAX, Address(ESP, EAX, TIMES_2, 0));  // EAX is Smi.
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(EDX);  // Preserve arguments descriptor array.
  __ pushl(ECX);  // Preserve IC data object.
  __ pushl(raw_null);  // Setup space on stack for result (target code object).
  // Push call arguments.
  for (intptr_t i = 0; i < num_args; i++) {
    __ movl(EBX, Address(EAX, -kWordSize * i));
    __ pushl(EBX);
  }
  __ pushl(ECX);  // Pass IC data object.
  __ pushl(EDX);  // Pass arguments descriptor array.
  if (num_args == 1) {
    __ CallRuntime(kInlineCacheMissHandlerOneArgRuntimeEntry);
  } else if (num_args == 2) {
    __ CallRuntime(kInlineCacheMissHandlerTwoArgsRuntimeEntry);
  } else if (num_args == 3) {
    __ CallRuntime(kInlineCacheMissHandlerThreeArgsRuntimeEntry);
  } else {
    UNIMPLEMENTED();
  }
  // Remove the call arguments pushed earlier, including the IC data object
  // and the arguments descriptor array.
  for (intptr_t i = 0; i < num_args + 2; i++) {
    __ popl(EAX);
  }
  __ popl(EAX);  // Pop returned code object into EAX (null if not found).
  __ popl(ECX);  // Restore IC data array.
  __ popl(EDX);  // Restore arguments descriptor array.
  __ LeaveFrame();
  Label call_target_function;
  __ cmpl(EAX, raw_null);
  __ j(NOT_EQUAL, &call_target_function, Assembler::kNearJump);
  // NoSuchMethod or closure.
  // Mark IC call that it may be a closure call that does not collect
  // type feedback.
  __ movb(FieldAddress(ECX, ICData::is_closure_call_offset()), Immediate(1));
  __ jmp(&StubCode::InstanceFunctionLookupLabel());

  __ Bind(&found);
  // EBX: Pointer to an IC data check group.
  const intptr_t target_offset = ICData::TargetIndexFor(num_args) * kWordSize;
  const intptr_t count_offset = ICData::CountIndexFor(num_args) * kWordSize;
  __ movl(EAX, Address(EBX, target_offset));
  __ addl(Address(EBX, count_offset), Immediate(Smi::RawValue(1)));
  __ j(NO_OVERFLOW, &call_target_function);
  __ movl(Address(EBX, count_offset),
          Immediate(Smi::RawValue(Smi::kMaxValue)));

  __ Bind(&call_target_function);
  // EAX: Target function.
  __ movl(EAX, FieldAddress(EAX, Function::code_offset()));
  __ movl(EAX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(EAX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ jmp(EAX);

  // Instance in EAX, return its class-id in EAX as Smi.
  __ Bind(&get_class_id_as_smi);
  Label not_smi;
  // Test if Smi -> load Smi class for comparison.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &not_smi, Assembler::kNearJump);
  __ movl(EAX, Immediate(Smi::RawValue(kSmiCid)));
  __ ret();

  __ Bind(&not_smi);
  __ LoadClassId(EAX, EAX);
  __ SmiTag(EAX);
  __ ret();
}


// Use inline cache data array to invoke the target or continue in inline
// cache miss handler. Stub for 1-argument check (receiver class).
//  ECX: Inline cache data object.
//  EDX: Arguments descriptor array.
//  TOS(0): Return address.
// Inline cache data object structure:
// 0: function-name
// 1: N, number of arguments checked.
// 2 .. (length - 1): group of checks, each check containing:
//   - N classes.
//   - 1 target function.
void StubCode::GenerateOneArgCheckInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, EBX);
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


void StubCode::GenerateTwoArgsCheckInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, EBX);
  GenerateNArgsCheckInlineCacheStub(assembler, 2);
}


void StubCode::GenerateThreeArgsCheckInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, EBX);
  GenerateNArgsCheckInlineCacheStub(assembler, 3);
}


// Use inline cache data array to invoke the target or continue in inline
// cache miss handler. Stub for 1-argument check (receiver class).
//  EDI: function which counter needs to be incremented.
//  ECX: Inline cache data object.
//  EDX: Arguments descriptor array.
//  TOS(0): Return address.
// Inline cache data object structure:
// 0: function-name
// 1: N, number of arguments checked.
// 2 .. (length - 1): group of checks, each check containing:
//   - N classes.
//   - 1 target function.
void StubCode::GenerateOneArgOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


void StubCode::GenerateTwoArgsOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 2);
}


void StubCode::GenerateThreeArgsOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 3);
}


// Do not count as no type feedback is collected.
void StubCode::GenerateClosureCallInlineCacheStub(Assembler* assembler) {
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


// Megamorphic call is currently implemented as IC call but through a stub
// that does not check/count function invocations.
void StubCode::GenerateMegamorphicCallStub(Assembler* assembler) {
  GenerateNArgsCheckInlineCacheStub(assembler, 1);
}


//  EDX: Arguments descriptor array.
//  TOS(0): return address (Dart code).
void StubCode::GenerateBreakpointStaticStub(Assembler* assembler) {
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(EDX);  // Preserve arguments descriptor.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ pushl(raw_null);  // Room for result.
  __ CallRuntime(kBreakpointStaticHandlerRuntimeEntry);
  __ popl(EAX);  // Code object.
  __ popl(EDX);  // Restore arguments descriptor.
  __ LeaveFrame();

  // Now call the static function. The breakpoint handler function
  // ensures that the call target is compiled.
  __ movl(ECX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(ECX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ jmp(ECX);
}


//  TOS(0): return address (Dart code).
void StubCode::GenerateBreakpointReturnStub(Assembler* assembler) {
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(EAX);
  __ CallRuntime(kBreakpointReturnHandlerRuntimeEntry);
  __ popl(EAX);
  __ LeaveFrame();

  // Instead of returning to the patched Dart function, emulate the
  // smashed return code pattern and return to the function's caller.
  __ popl(ECX);  // Discard return address to patched dart code.
  // Execute function epilog code that was smashed in the Dart code.
  __ LeaveFrame();
  __ ret();
}


//  ECX: Inline cache data array.
//  EDX: Arguments descriptor array.
//  TOS(0): return address (Dart code).
void StubCode::GenerateBreakpointDynamicStub(Assembler* assembler) {
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  __ pushl(ECX);
  __ pushl(EDX);
  __ CallRuntime(kBreakpointDynamicHandlerRuntimeEntry);
  __ popl(EDX);
  __ popl(ECX);
  __ LeaveFrame();

  // Find out which dispatch stub to call.
  Label test_two, test_three, test_four;
  __ movl(EBX, FieldAddress(ECX, ICData::num_args_tested_offset()));
  __ cmpl(EBX, Immediate(1));
  __ j(NOT_EQUAL, &test_two, Assembler::kNearJump);
  __ jmp(&StubCode::OneArgCheckInlineCacheLabel());
  __ Bind(&test_two);
  __ cmpl(EBX, Immediate(2));
  __ j(NOT_EQUAL, &test_three, Assembler::kNearJump);
  __ jmp(&StubCode::TwoArgsCheckInlineCacheLabel());
  __ Bind(&test_three);
  __ cmpl(EBX, Immediate(3));
  __ j(NOT_EQUAL, &test_four, Assembler::kNearJump);
  __ jmp(&StubCode::ThreeArgsCheckInlineCacheLabel());
  __ Bind(&test_four);
  __ Stop("Unsupported number of arguments tested.");
}


// Used to check class and type arguments. Arguments passed on stack:
// TOS + 0: return address.
// TOS + 1: instantiator type arguments (can be NULL).
// TOS + 2: instance.
// TOS + 3: SubtypeTestCache.
// Result in ECX: null -> not found, otherwise result (true or false).
static void GenerateSubtypeNTestCacheStub(Assembler* assembler, int n) {
  ASSERT((1 <= n) && (n <= 3));
  const intptr_t kInstantiatorTypeArgumentsInBytes = 1 * kWordSize;
  const intptr_t kInstanceOffsetInBytes = 2 * kWordSize;
  const intptr_t kCacheOffsetInBytes = 3 * kWordSize;
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ movl(EAX, Address(ESP, kInstanceOffsetInBytes));
  if (n > 1) {
    // Get instance type arguments.
    __ LoadClass(ECX, EAX, EBX);
    // Compute instance type arguments into EBX.
    Label has_no_type_arguments;
    __ movl(EBX, raw_null);
    __ movl(EDI, FieldAddress(ECX,
        Class::type_arguments_field_offset_in_words_offset()));
    __ cmpl(EDI, Immediate(Class::kNoTypeArguments));
    __ j(EQUAL, &has_no_type_arguments, Assembler::kNearJump);
    __ movl(EBX, FieldAddress(EAX, EDI, TIMES_4, 0));
    __ Bind(&has_no_type_arguments);
  }
  __ LoadClassId(ECX, EAX);
  // EAX: instance, ECX: instance class id.
  // EBX: instance type arguments (null if none), used only if n > 1.
  __ movl(EDX, Address(ESP, kCacheOffsetInBytes));
  // EDX: SubtypeTestCache.
  __ movl(EDX, FieldAddress(EDX, SubtypeTestCache::cache_offset()));
  __ addl(EDX, Immediate(Array::data_offset() - kHeapObjectTag));

  Label loop, found, not_found, next_iteration;
  // EDX: Entry start.
  // ECX: instance class id.
  // EBX: instance type arguments.
  __ SmiTag(ECX);
  __ Bind(&loop);
  __ movl(EDI, Address(EDX, kWordSize * SubtypeTestCache::kInstanceClassId));
  __ cmpl(EDI, raw_null);
  __ j(EQUAL, &not_found, Assembler::kNearJump);
  __ cmpl(EDI, ECX);
  if (n == 1) {
    __ j(EQUAL, &found, Assembler::kNearJump);
  } else {
    __ j(NOT_EQUAL, &next_iteration, Assembler::kNearJump);
    __ movl(EDI,
          Address(EDX, kWordSize * SubtypeTestCache::kInstanceTypeArguments));
    __ cmpl(EDI, EBX);
    if (n == 2) {
      __ j(EQUAL, &found, Assembler::kNearJump);
    } else {
      __ j(NOT_EQUAL, &next_iteration, Assembler::kNearJump);
      __ movl(EDI,
              Address(EDX, kWordSize *
                           SubtypeTestCache::kInstantiatorTypeArguments));
      __ cmpl(EDI, Address(ESP, kInstantiatorTypeArgumentsInBytes));
      __ j(EQUAL, &found, Assembler::kNearJump);
    }
  }
  __ Bind(&next_iteration);
  __ addl(EDX, Immediate(kWordSize * SubtypeTestCache::kTestEntryLength));
  __ jmp(&loop, Assembler::kNearJump);
  // Fall through to not found.
  __ Bind(&not_found);
  __ movl(ECX, raw_null);
  __ ret();

  __ Bind(&found);
  __ movl(ECX, Address(EDX, kWordSize * SubtypeTestCache::kTestResult));
  __ ret();
}


// Used to check class and type arguments. Arguments passed on stack:
// TOS + 0: return address.
// TOS + 1: instantiator type arguments or NULL.
// TOS + 2: instance.
// TOS + 3: cache array.
// Result in ECX: null -> not found, otherwise result (true or false).
void StubCode::GenerateSubtype1TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 1);
}


// Used to check class and type arguments. Arguments passed on stack:
// TOS + 0: return address.
// TOS + 1: instantiator type arguments or NULL.
// TOS + 2: instance.
// TOS + 3: cache array.
// Result in ECX: null -> not found, otherwise result (true or false).
void StubCode::GenerateSubtype2TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 2);
}


// Used to check class and type arguments. Arguments passed on stack:
// TOS + 0: return address.
// TOS + 1: instantiator type arguments.
// TOS + 2: instance.
// TOS + 3: cache array.
// Result in ECX: null -> not found, otherwise result (true or false).
void StubCode::GenerateSubtype3TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 3);
}


// Return the current stack pointer address, used to stack alignment
// checks.
// TOS + 0: return address
// Result in EAX.
void StubCode::GenerateGetStackPointerStub(Assembler* assembler) {
  __ leal(EAX, Address(ESP, kWordSize));
  __ ret();
}


// Jump to the exception handler.
// TOS + 0: return address
// TOS + 1: program_counter
// TOS + 2: stack_pointer
// TOS + 3: frame_pointer
// TOS + 4: exception object
// TOS + 5: stacktrace object
// No Result.
void StubCode::GenerateJumpToExceptionHandlerStub(Assembler* assembler) {
  ASSERT(kExceptionObjectReg == EAX);
  ASSERT(kStackTraceObjectReg == EDX);
  __ movl(kStackTraceObjectReg, Address(ESP, 5 * kWordSize));
  __ movl(kExceptionObjectReg, Address(ESP, 4 * kWordSize));
  __ movl(EBP, Address(ESP, 3 * kWordSize));  // Load target frame_pointer.
  __ movl(EBX, Address(ESP, 1 * kWordSize));  // Load target PC into EBX.
  __ movl(ESP, Address(ESP, 2 * kWordSize));  // Load target stack_pointer.
  __ jmp(EBX);  // Jump to the exception handler code.
}


// Jump to the error handler.
// TOS + 0: return address
// TOS + 1: program_counter
// TOS + 2: stack_pointer
// TOS + 3: frame_pointer
// TOS + 4: error object
// No Result.
void StubCode::GenerateJumpToErrorHandlerStub(Assembler* assembler) {
  __ movl(EAX, Address(ESP, 4 * kWordSize));  // Load error object.
  __ movl(EBP, Address(ESP, 3 * kWordSize));  // Load target frame_pointer.
  __ movl(EBX, Address(ESP, 1 * kWordSize));  // Load target PC into EBX.
  __ movl(ESP, Address(ESP, 2 * kWordSize));  // Load target stack_pointer.
  __ jmp(EBX);  // Jump to the exception handler code.
}


// Implements equality operator when one of the arguments is null
// (identity check) and updates ICData if necessary.
// TOS + 0: return address
// TOS + 1: right argument
// TOS + 2: left argument
// ECX: ICData.
// EAX: result.
// TODO(srdjan): Move to VM stubs once Boolean objects become VM objects.
void StubCode::GenerateEqualityWithNullArgStub(Assembler* assembler) {
  static const intptr_t kNumArgsTested = 2;
#if defined(DEBUG)
  { Label ok;
    __ movl(EAX, FieldAddress(ECX, ICData::num_args_tested_offset()));
    __ cmpl(EAX, Immediate(kNumArgsTested));
    __ j(EQUAL, &ok, Assembler::kNearJump);
    __ Stop("Incorrect ICData for equality");
    __ Bind(&ok);
  }
#endif  // DEBUG
  // Check IC data, update if needed.
  // EBX: IC data object (preserved).
  __ movl(EBX, FieldAddress(ECX, ICData::ic_data_offset()));
  // EBX: ic_data_array with check entries: classes and target functions.
  __ leal(EBX, FieldAddress(EBX, Array::data_offset()));
  // EBX: points directly to the first ic data array element.

  Label get_class_id_as_smi, no_match, loop, compute_result, found;
  __ Bind(&loop);
  // Check left.
  __ movl(EAX, Address(ESP, 2 * kWordSize));
  __ call(&get_class_id_as_smi);
  __ movl(EDI, Address(EBX, 0 * kWordSize));
  __ cmpl(EAX, EDI);  // Class id match?
  __ j(NOT_EQUAL, &no_match, Assembler::kNearJump);
  // Check right.
  __ movl(EAX, Address(ESP, 1 * kWordSize));
  __ call(&get_class_id_as_smi);
  __ movl(EDI, Address(EBX, 1 * kWordSize));
  __ cmpl(EAX, EDI);  // Class id match?
  __ j(EQUAL, &found, Assembler::kNearJump);
  __ Bind(&no_match);
  // Next check group.
  __ addl(EBX, Immediate(
      kWordSize * ICData::TestEntryLengthFor(kNumArgsTested)));
  __ cmpl(EDI, Immediate(Smi::RawValue(kIllegalCid)));  // Done?
  __ j(NOT_EQUAL, &loop, Assembler::kNearJump);
  Label update_ic_data;
  __ jmp(&update_ic_data);

  __ Bind(&found);
  const intptr_t count_offset =
      ICData::CountIndexFor(kNumArgsTested) * kWordSize;
  __ addl(Address(EBX, count_offset), Immediate(Smi::RawValue(1)));
  __ j(NO_OVERFLOW, &compute_result);
  __ movl(Address(EBX, count_offset),
          Immediate(Smi::RawValue(Smi::kMaxValue)));

  __ Bind(&compute_result);
  Label true_label;
  __ movl(EAX, Address(ESP, 1 * kWordSize));
  __ cmpl(EAX, Address(ESP, 2 * kWordSize));
  __ j(EQUAL, &true_label, Assembler::kNearJump);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&true_label);
  __ LoadObject(EAX, Bool::True());
  __ ret();

  __ Bind(&get_class_id_as_smi);
  Label not_smi;
  // Test if Smi -> load Smi class for comparison.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &not_smi, Assembler::kNearJump);
  __ movl(EAX, Immediate(Smi::RawValue(kSmiCid)));
  __ ret();

  __ Bind(&not_smi);
  __ LoadClassId(EAX, EAX);
  __ SmiTag(EAX);
  __ ret();

  __ Bind(&update_ic_data);

  // ECX: ICData
  __ movl(EAX, Address(ESP, 1 * kWordSize));
  __ movl(EDI, Address(ESP, 2 * kWordSize));
  __ EnterStubFrame();
  __ pushl(EDI);  // arg 0
  __ pushl(EAX);  // arg 1
  __ PushObject(Symbols::EqualOperator());  // Target's name.
  __ pushl(ECX);  // ICData
  __ CallRuntime(kUpdateICDataTwoArgsRuntimeEntry);
  __ Drop(4);
  __ LeaveFrame();

  __ jmp(&compute_result, Assembler::kNearJump);
}


// Calls to the runtime to optimize the given function.
// EDI: function to be reoptimized.
// EDX: argument descriptor (preserved).
void StubCode::GenerateOptimizeFunctionStub(Assembler* assembler) {
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ EnterStubFrame();
  __ pushl(EDX);
  __ pushl(raw_null);  // Setup space on stack for return value.
  __ pushl(EDI);
  __ CallRuntime(kOptimizeInvokedFunctionRuntimeEntry);
  __ popl(EAX);  // Discard argument.
  __ popl(EAX);  // Get Code object
  __ popl(EDX);  // Restore argument descriptor.
  __ movl(EAX, FieldAddress(EAX, Code::instructions_offset()));
  __ addl(EAX, Immediate(Instructions::HeaderSize() - kHeapObjectTag));
  __ LeaveFrame();
  __ jmp(EAX);
  __ int3();
}


DECLARE_LEAF_RUNTIME_ENTRY(intptr_t,
                           BigintCompare,
                           RawBigint* left,
                           RawBigint* right);


// Does identical check (object references are equal or not equal) with special
// checks for boxed numbers.
// Left and right are pushed on stack.
// Return ZF set.
// Note: A Mint cannot contain a value that would fit in Smi, a Bigint
// cannot contain a value that fits in Mint or Smi.
void StubCode::GenerateIdenticalWithNumberCheckStub(Assembler* assembler) {
  const Register left = EAX;
  const Register right = EDX;
  const Register temp = ECX;
  // Preserve left, right and temp.
  __ pushl(left);
  __ pushl(right);
  __ pushl(temp);
  // TOS + 0: saved temp
  // TOS + 1: saved right
  // TOS + 2: saved left
  // TOS + 3: return address
  // TOS + 4: right argument.
  // TOS + 5: left argument.
  __ movl(left, Address(ESP, 5 * kWordSize));
  __ movl(right, Address(ESP, 4 * kWordSize));
  Label reference_compare, done, check_mint, check_bigint;
  // If any of the arguments is Smi do reference compare.
  __ testl(left, Immediate(kSmiTagMask));
  __ j(ZERO, &reference_compare, Assembler::kNearJump);
  __ testl(right, Immediate(kSmiTagMask));
  __ j(ZERO, &reference_compare, Assembler::kNearJump);

  // Value compare for two doubles.
  __ CompareClassId(left, kDoubleCid, temp);
  __ j(NOT_EQUAL, &check_mint, Assembler::kNearJump);
  __ CompareClassId(right, kDoubleCid, temp);
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);

  // Double values bitwise compare.
  __ movl(temp, FieldAddress(left, Double::value_offset() + 0 * kWordSize));
  __ cmpl(temp, FieldAddress(right, Double::value_offset() + 0 * kWordSize));
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ movl(temp, FieldAddress(left, Double::value_offset() + 1 * kWordSize));
  __ cmpl(temp, FieldAddress(right, Double::value_offset() + 1 * kWordSize));
  __ jmp(&done, Assembler::kNearJump);

  __ Bind(&check_mint);
  __ CompareClassId(left, kMintCid, temp);
  __ j(NOT_EQUAL, &check_bigint, Assembler::kNearJump);
  __ CompareClassId(right, kMintCid, temp);
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ movl(temp, FieldAddress(left, Mint::value_offset() + 0 * kWordSize));
  __ cmpl(temp, FieldAddress(right, Mint::value_offset() + 0 * kWordSize));
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ movl(temp, FieldAddress(left, Mint::value_offset() + 1 * kWordSize));
  __ cmpl(temp, FieldAddress(right, Mint::value_offset() + 1 * kWordSize));
  __ jmp(&done, Assembler::kNearJump);

  __ Bind(&check_bigint);
  __ CompareClassId(left, kBigintCid, temp);
  __ j(NOT_EQUAL, &reference_compare, Assembler::kNearJump);
  __ CompareClassId(right, kBigintCid, temp);
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ EnterFrame(0);
  __ ReserveAlignedFrameSpace(2 * kWordSize);
  __ movl(Address(ESP, 1 * kWordSize), left);
  __ movl(Address(ESP, 0 * kWordSize), right);
  __ CallRuntime(kBigintCompareRuntimeEntry);
  // Result in EAX, 0 means equal.
  __ LeaveFrame();
  __ cmpl(EAX, Immediate(0));
  __ jmp(&done);

  __ Bind(&reference_compare);
  __ cmpl(left, right);
  __ Bind(&done);
  __ popl(temp);
  __ popl(right);
  __ popl(left);
  __ ret();
}

}  // namespace dart

#endif  // defined TARGET_ARCH_IA32
