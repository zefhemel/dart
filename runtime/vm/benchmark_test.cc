// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/benchmark_test.h"

#include "bin/file.h"

#include "platform/assert.h"

#include "vm/dart_api_impl.h"
#include "vm/stack_frame.h"
#include "vm/unit_test.h"

namespace dart {

Benchmark* Benchmark::first_ = NULL;
Benchmark* Benchmark::tail_ = NULL;
const char* Benchmark::executable_ = NULL;

void Benchmark::RunAll(const char* executable) {
  SetExecutable(executable);
  Benchmark* benchmark = first_;
  while (benchmark != NULL) {
    benchmark->RunBenchmark();
    benchmark = benchmark->next_;
  }
}


// Compiler only implemented on IA32 and X64 now.
#if defined(TARGET_ARCH_IA32) || defined(TARGET_ARCH_X64)


//
// Measure compile of all functions in dart core lib classes.
//
BENCHMARK(CorelibCompileAll) {
  Timer timer(true, "Compile all of Core lib benchmark");
  timer.Start();
  const Error& error = Error::Handle(benchmark->isolate(),
                                     Library::CompileAll());
  if (!error.IsNull()) {
    OS::PrintErr("Unexpected error in CorelibCompileAll benchmark:\n%s",
                 error.ToErrorCString());
  }
  EXPECT(error.IsNull());
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  benchmark->set_score(elapsed_time);
}

#endif  // TARGET_ARCH_IA32 || TARGET_ARCH_X64


//
// Measure creation of core isolate from a snapshot.
//
BENCHMARK(CorelibIsolateStartup) {
  const int kNumIterations = 100;
  char* err = NULL;
  Dart_Isolate base_isolate = Dart_CurrentIsolate();
  Dart_Isolate test_isolate = Dart_CreateIsolate(NULL, NULL, NULL, NULL, &err);
  EXPECT(test_isolate != NULL);
  Dart_EnterScope();
  uint8_t* buffer = NULL;
  intptr_t size = 0;
  Dart_Handle result = Dart_CreateSnapshot(&buffer, &size);
  EXPECT_VALID(result);
  Timer timer(true, "Core Isolate startup benchmark");
  timer.Start();
  for (int i = 0; i < kNumIterations; i++) {
    Dart_Isolate new_isolate =
        Dart_CreateIsolate(NULL, NULL, buffer, NULL, &err);
    EXPECT(new_isolate != NULL);
    Dart_ShutdownIsolate();
  }
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  benchmark->set_score(elapsed_time / kNumIterations);
  Dart_EnterIsolate(test_isolate);
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  Dart_EnterIsolate(base_isolate);
}


//
// Measure invocation of Dart API functions.
//
static void InitNativeFields(Dart_NativeArguments args) {
  Dart_EnterScope();
  int count = Dart_GetNativeArgumentCount(args);
  EXPECT_EQ(1, count);

  Dart_Handle recv = Dart_GetNativeArgument(args, 0);
  EXPECT_VALID(recv);
  Dart_Handle result = Dart_SetNativeInstanceField(recv, 0, 7);
  EXPECT_VALID(result);

  Dart_ExitScope();
}


// The specific api functions called here are a bit arbitrary.  We are
// trying to get a sense of the overhead for using the dart api.
static void UseDartApi(Dart_NativeArguments args) {
  Dart_EnterScope();
  int count = Dart_GetNativeArgumentCount(args);
  EXPECT_EQ(3, count);

  // Get the receiver.
  Dart_Handle recv = Dart_GetNativeArgument(args, 0);
  EXPECT_VALID(recv);

  // Get param1.
  Dart_Handle param1 = Dart_GetNativeArgument(args, 1);
  EXPECT_VALID(param1);
  EXPECT(Dart_IsInteger(param1));
  bool fits = false;
  Dart_Handle result = Dart_IntegerFitsIntoInt64(param1, &fits);
  EXPECT_VALID(result);
  EXPECT(fits);
  int64_t value1;
  result = Dart_IntegerToInt64(param1, &value1);
  EXPECT_VALID(result);
  EXPECT_LE(0, value1);
  EXPECT_LE(value1, 1000000);

  // Get native field from receiver.
  intptr_t value2;
  result = Dart_GetNativeInstanceField(recv, 0, &value2);
  EXPECT_VALID(result);
  EXPECT_EQ(7, value2);

  // Return param + receiver.field.
  Dart_SetReturnValue(args, Dart_NewInteger(value1 * value2));
  Dart_ExitScope();
}


static Dart_NativeFunction bm_uda_lookup(Dart_Handle name, int argument_count) {
  const char* cstr = NULL;
  Dart_Handle result = Dart_StringToCString(name, &cstr);
  EXPECT_VALID(result);
  if (strcmp(cstr, "init") == 0) {
    return InitNativeFields;
  } else {
    return UseDartApi;
  }
}


BENCHMARK(UseDartApi) {
  const int kNumIterations = 1000000;
  const char* kScriptChars =
      "class Class extends NativeFieldsWrapper{\n"
      "  int init() native 'init';\n"
      "  int method(int param1, int param2) native 'method';\n"
      "}\n"
      "\n"
      "void benchmark(int count) {\n"
      "  Class c = new Class();\n"
      "  c.init();\n"
      "  for (int i = 0; i < count; i++) {\n"
      "    c.method(i,7);\n"
      "  }\n"
      "}\n";

  Dart_Handle lib = TestCase::LoadTestScript(
      kScriptChars,
      reinterpret_cast<Dart_NativeEntryResolver>(bm_uda_lookup));

  // Create a native wrapper class with native fields.
  Dart_Handle result = Dart_CreateNativeWrapperClass(
      lib, NewString("NativeFieldsWrapper"), 1);
  EXPECT_VALID(result);

  Dart_Handle args[1];
  args[0] = Dart_NewInteger(kNumIterations);

  // Warmup first to avoid compilation jitters.
  Dart_Invoke(lib, NewString("benchmark"), 1, args);

  Timer timer(true, "UseDartApi benchmark");
  timer.Start();
  Dart_Invoke(lib, NewString("benchmark"), 1, args);
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  benchmark->set_score(elapsed_time);
}


//
// Measure time accessing internal and external strings.
//
BENCHMARK(DartStringAccess) {
  const int kNumIterations = 10000000;
  Timer timer(true, "DartStringAccess benchmark");
  timer.Start();
  Dart_EnterScope();

  // Create strings.
  uint8_t data8[] = { 'o', 'n', 'e', 0xFF };
  int external_peer_data = 123;
  Dart_Handle external_string = Dart_NewExternalLatin1String(
      data8, ARRAY_SIZE(data8), &external_peer_data, NULL);
  Dart_Handle internal_string = NewString("two");

  // Run benchmark.
  for (int64_t i = 0; i < kNumIterations; i++) {
    EXPECT(Dart_IsString(internal_string));
    EXPECT(!Dart_IsExternalString(internal_string));
    EXPECT_VALID(external_string);
    EXPECT(Dart_IsExternalString(external_string));
    void* external_peer = NULL;
    EXPECT_VALID(Dart_ExternalStringGetPeer(external_string, &external_peer));
    EXPECT_EQ(&external_peer_data, external_peer);
  }

  Dart_ExitScope();
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  benchmark->set_score(elapsed_time);
}


//
// Measure compile of all dart2js(compiler) functions.
//
static char* ComputeDart2JSPath(const char* arg) {
  char buffer[2048];
  char* dart2js_path = strdup(File::GetCanonicalPath(arg));
  const char* compiler_path =
      "%s%ssdk%slib%s_internal%scompiler%scompiler.dart";
  const char* path_separator = File::PathSeparator();
  ASSERT(path_separator != NULL && strlen(path_separator) == 1);
  char* ptr = strrchr(dart2js_path, *path_separator);
  while (ptr != NULL) {
    *ptr = '\0';
    OS::SNPrint(buffer, 2048, compiler_path,
                dart2js_path,
                path_separator,
                path_separator,
                path_separator,
                path_separator,
                path_separator);
    if (File::Exists(buffer)) {
      break;
    }
    ptr = strrchr(dart2js_path, *path_separator);
  }
  if (ptr == NULL) {
    free(dart2js_path);
    dart2js_path = NULL;
  }
  return dart2js_path;
}


static void func(Dart_NativeArguments args) {
}


static Dart_NativeFunction NativeResolver(Dart_Handle name,
                                          int arg_count) {
  return &func;
}


BENCHMARK(Dart2JSCompileAll) {
  char* dart_root = ComputeDart2JSPath(Benchmark::Executable());
  char* script = NULL;
  if (dart_root != NULL) {
    Isolate* isolate = Isolate::Current();
    HANDLESCOPE(isolate);
    const char* kFormatStr =
        "import '%s/sdk/lib/_internal/compiler/compiler.dart';";
    intptr_t len = OS::SNPrint(NULL, 0, kFormatStr, dart_root) + 1;
    script = reinterpret_cast<char*>(malloc(len));
    EXPECT(script != NULL);
    OS::SNPrint(script, len, kFormatStr, dart_root);
    Dart_Handle lib = TestCase::LoadTestScript(
        script,
        reinterpret_cast<Dart_NativeEntryResolver>(NativeResolver));
    EXPECT_VALID(lib);
  } else {
    Dart_Handle lib = TestCase::LoadTestScript(
        "import 'sdk/lib/_internal/compiler/compiler.dart';",
        reinterpret_cast<Dart_NativeEntryResolver>(NativeResolver));
    EXPECT_VALID(lib);
  }
  Timer timer(true, "Compile all of dart2js benchmark");
  timer.Start();
  Dart_Handle result = Dart_CompileAll();
  EXPECT_VALID(result);
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  benchmark->set_score(elapsed_time);
  free(dart_root);
  free(script);
}


//
// Measure frame lookup during stack traversal.
//
static void StackFrame_accessFrame(Dart_NativeArguments args) {
  const int kNumIterations = 100;
  Dart_EnterScope();
  Code& code = Code::Handle();
  Timer timer(true, "LookupDartCode benchmark");
  timer.Start();
  for (int i = 0; i < kNumIterations; i++) {
    StackFrameIterator frames(StackFrameIterator::kDontValidateFrames);
    StackFrame* frame = frames.NextFrame();
    while (frame != NULL) {
      if (frame->IsStubFrame()) {
        code = frame->LookupDartCode();
        EXPECT(code.function() == Function::null());
      } else if (frame->IsDartFrame()) {
        code = frame->LookupDartCode();
        EXPECT(code.function() != Function::null());
      }
      frame = frames.NextFrame();
    }
  }
  timer.Stop();
  int64_t elapsed_time = timer.TotalElapsedTime();
  Dart_SetReturnValue(args, Dart_NewInteger(elapsed_time));
  Dart_ExitScope();
}


static Dart_NativeFunction StackFrameNativeResolver(Dart_Handle name,
                                                    int arg_count) {
  return &StackFrame_accessFrame;
}


// Unit test case to verify stack frame iteration.
BENCHMARK(FrameLookup) {
  const char* kScriptChars =
      "class StackFrame {"
      "  static int accessFrame() native \"StackFrame_accessFrame\";"
      "} "
      "class First {"
      "  First() { }"
      "  int method1(int param) {"
      "    if (param == 1) {"
      "      param = method2(200);"
      "    } else {"
      "      param = method2(100);"
      "    }"
      "    return param;"
      "  }"
      "  int method2(int param) {"
      "    if (param == 200) {"
      "      return First.staticmethod(this, param);"
      "    } else {"
      "      return First.staticmethod(this, 10);"
      "    }"
      "  }"
      "  static int staticmethod(First obj, int param) {"
      "    if (param == 10) {"
      "      return obj.method3(10);"
      "    } else {"
      "      return obj.method3(200);"
      "    }"
      "  }"
      "  int method3(int param) {"
      "    return StackFrame.accessFrame();"
      "  }"
      "}"
      "class StackFrameTest {"
      "  static int testMain() {"
      "    First obj = new First();"
      "    return obj.method1(1);"
      "  }"
      "}";
  Dart_Handle lib = TestCase::LoadTestScript(
      kScriptChars,
      reinterpret_cast<Dart_NativeEntryResolver>(StackFrameNativeResolver));
  Dart_Handle cls = Dart_GetClass(lib, NewString("StackFrameTest"));
  Dart_Handle result = Dart_Invoke(cls, NewString("testMain"), 0, NULL);
  EXPECT_VALID(result);
  int64_t elapsed_time = 0;
  result = Dart_IntegerToInt64(result, &elapsed_time);
  EXPECT_VALID(result);
  benchmark->set_score(elapsed_time);
}


static uint8_t* malloc_allocator(
    uint8_t* ptr, intptr_t old_size, intptr_t new_size) {
  return reinterpret_cast<uint8_t*>(realloc(ptr, new_size));
}


BENCHMARK(CoreSnapshotSize) {
  const char* kScriptChars =
      "import 'dart:async';\n"
      "import 'dart:core';\n"
      "import 'dart:collection';\n"
      "import 'dart:_collection-dev';\n"
      "import 'dart:math';\n"
      "import 'dart:isolate';\n"
      "import 'dart:mirrors';\n"
      "import 'dart:typeddata';\n"
      "\n";

  // Start an Isolate, load a script and create a full snapshot.
  uint8_t* buffer;
  TestCase::LoadTestScript(kScriptChars, NULL);
  Api::CheckIsolateState(Isolate::Current());

  // Write snapshot with object content.
  FullSnapshotWriter writer(&buffer, &malloc_allocator);
  writer.WriteFullSnapshot();
  const Snapshot* snapshot = Snapshot::SetupFromBuffer(buffer);
  ASSERT(snapshot->kind() == Snapshot::kFull);
  benchmark->set_score(snapshot->length());
}


BENCHMARK(StandaloneSnapshotSize) {
  const char* kScriptChars =
      "import 'dart:async';\n"
      "import 'dart:core';\n"
      "import 'dart:collection';\n"
      "import 'dart:_collection-dev';\n"
      "import 'dart:math';\n"
      "import 'dart:isolate';\n"
      "import 'dart:mirrors';\n"
      "import 'dart:typeddata';\n"
      "import 'dart:uri';\n"
      "import 'dart:utf';\n"
      "import 'dart:json';\n"
      "import 'dart:crypto';\n"
      "import 'dart:builtin';\n"
      "import 'dart:io';\n"
      "\n";

  // Start an Isolate, load a script and create a full snapshot.
  uint8_t* buffer;
  TestCase::LoadTestScript(kScriptChars, NULL);
  Api::CheckIsolateState(Isolate::Current());

  // Write snapshot with object content.
  FullSnapshotWriter writer(&buffer, &malloc_allocator);
  writer.WriteFullSnapshot();
  const Snapshot* snapshot = Snapshot::SetupFromBuffer(buffer);
  ASSERT(snapshot->kind() == Snapshot::kFull);
  benchmark->set_score(snapshot->length());
}

}  // namespace dart
