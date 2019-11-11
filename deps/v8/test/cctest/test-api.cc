// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <climits>
#include <csignal>
#include <map>
#include <memory>
#include <string>

#include "test/cctest/test-api.h"

#if V8_OS_POSIX
#include <unistd.h>  // NOLINT
#endif

#include "include/v8-util.h"
#include "src/api/api-inl.h"
#include "src/base/overflowing-math.h"
#include "src/base/platform/platform.h"
#include "src/codegen/compilation-cache.h"
#include "src/debug/debug.h"
#include "src/execution/arguments.h"
#include "src/execution/execution.h"
#include "src/execution/futex-emulation.h"
#include "src/execution/vm-state.h"
#include "src/handles/global-handles.h"
#include "src/heap/heap-inl.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/local-allocator.h"
#include "src/objects/feedback-vector-inl.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/lookup.h"
#include "src/objects/module-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/string-inl.h"
#include "src/profiler/cpu-profiler.h"
#include "src/strings/unicode-inl.h"
#include "src/utils/utils.h"
#include "src/wasm/wasm-js.h"
#include "test/cctest/heap/heap-tester.h"
#include "test/cctest/heap/heap-utils.h"
#include "test/cctest/wasm/wasm-run-utils.h"
#include "test/common/wasm/wasm-macro-gen.h"

static const bool kLogThreading = false;

using ::v8::Array;
using ::v8::Boolean;
using ::v8::BooleanObject;
using ::v8::Context;
using ::v8::Extension;
using ::v8::Function;
using ::v8::FunctionTemplate;
using ::v8::HandleScope;
using ::v8::Local;
using ::v8::Maybe;
using ::v8::Message;
using ::v8::MessageCallback;
using ::v8::Module;
using ::v8::Name;
using ::v8::None;
using ::v8::Object;
using ::v8::ObjectTemplate;
using ::v8::Persistent;
using ::v8::PropertyAttribute;
using ::v8::Script;
using ::v8::String;
using ::v8::Symbol;
using ::v8::TryCatch;
using ::v8::Undefined;
using ::v8::V8;
using ::v8::Value;


#define THREADED_PROFILED_TEST(Name)                                 \
  static void Test##Name();                                          \
  TEST(Name##WithProfiler) {                                         \
    RunWithProfiler(&Test##Name);                                    \
  }                                                                  \
  THREADED_TEST(Name)

void RunWithProfiler(void (*test)()) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<v8::String> profile_name = v8_str("my_profile1");
  v8::CpuProfiler* cpu_profiler = v8::CpuProfiler::New(env->GetIsolate());
  cpu_profiler->StartProfiling(profile_name);
  (*test)();
  reinterpret_cast<i::CpuProfiler*>(cpu_profiler)->DeleteAllProfiles();
  cpu_profiler->Dispose();
}


static int signature_callback_count;
static Local<Value> signature_expected_receiver;
static void IncrementingSignatureCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  signature_callback_count++;
  CHECK(signature_expected_receiver->Equals(
                                       args.GetIsolate()->GetCurrentContext(),
                                       args.Holder())
            .FromJust());
  CHECK(signature_expected_receiver->Equals(
                                       args.GetIsolate()->GetCurrentContext(),
                                       args.This())
            .FromJust());
  v8::Local<v8::Array> result =
      v8::Array::New(args.GetIsolate(), args.Length());
  for (int i = 0; i < args.Length(); i++) {
    CHECK(result->Set(args.GetIsolate()->GetCurrentContext(),
                      v8::Integer::New(args.GetIsolate(), i), args[i])
              .FromJust());
  }
  args.GetReturnValue().Set(result);
}


static void Returns42(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(42);
}


// Tests that call v8::V8::Dispose() cannot be threaded.
UNINITIALIZED_TEST(InitializeAndDisposeOnce) {
  CHECK(v8::V8::Initialize());
  CHECK(v8::V8::Dispose());
}


// Tests that call v8::V8::Dispose() cannot be threaded.
UNINITIALIZED_TEST(InitializeAndDisposeMultiple) {
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Initialize());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Initialize());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
}

// Tests that Smi::kZero is set up properly.
UNINITIALIZED_TEST(SmiZero) { CHECK_EQ(i::Smi::kZero, i::Smi::kZero); }

THREADED_TEST(Handles) {
  v8::HandleScope scope(CcTest::isolate());
  Local<Context> local_env;
  {
    LocalContext env;
    local_env = env.local();
  }

  // Local context should still be live.
  CHECK(!local_env.IsEmpty());
  local_env->Enter();

  v8::Local<v8::Primitive> undef = v8::Undefined(CcTest::isolate());
  CHECK(!undef.IsEmpty());
  CHECK(undef->IsUndefined());

  const char* source = "1 + 2 + 3";
  Local<Script> script = v8_compile(source);
  CHECK_EQ(6, v8_run_int32value(script));

  local_env->Exit();
}


THREADED_TEST(IsolateOfContext) {
  v8::HandleScope scope(CcTest::isolate());
  v8::Local<Context> env = Context::New(CcTest::isolate());

  CHECK(!env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
  env->Enter();
  CHECK(env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
  env->Exit();
  CHECK(!env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
}

static void TestSignatureLooped(const char* operation, Local<Value> receiver,
                                v8::Isolate* isolate) {
  i::ScopedVector<char> source(200);
  i::SNPrintF(source,
              "for (var i = 0; i < 10; i++) {"
              "  %s"
              "}",
              operation);
  signature_callback_count = 0;
  signature_expected_receiver = receiver;
  bool expected_to_throw = receiver.IsEmpty();
  v8::TryCatch try_catch(isolate);
  CompileRun(source.begin());
  CHECK_EQ(expected_to_throw, try_catch.HasCaught());
  if (!expected_to_throw) {
    CHECK_EQ(10, signature_callback_count);
  } else {
    CHECK(v8_str("TypeError: Illegal invocation")
              ->Equals(isolate->GetCurrentContext(),
                       try_catch.Exception()
                           ->ToString(isolate->GetCurrentContext())
                           .ToLocalChecked())
              .FromJust());
  }
}

static void TestSignatureOptimized(const char* operation, Local<Value> receiver,
                                   v8::Isolate* isolate) {
  i::ScopedVector<char> source(200);
  i::SNPrintF(source,
              "function test() {"
              "  %s"
              "};"
              "%%PrepareFunctionForOptimization(test);"
              "try { test() } catch(e) {}"
              "try { test() } catch(e) {}"
              "%%OptimizeFunctionOnNextCall(test);"
              "test()",
              operation);
  signature_callback_count = 0;
  signature_expected_receiver = receiver;
  bool expected_to_throw = receiver.IsEmpty();
  v8::TryCatch try_catch(isolate);
  CompileRun(source.begin());
  CHECK_EQ(expected_to_throw, try_catch.HasCaught());
  if (!expected_to_throw) {
    CHECK_EQ(3, signature_callback_count);
  } else {
    CHECK(v8_str("TypeError: Illegal invocation")
              ->Equals(isolate->GetCurrentContext(),
                       try_catch.Exception()
                           ->ToString(isolate->GetCurrentContext())
                           .ToLocalChecked())
              .FromJust());
  }
}

static void TestSignature(const char* operation, Local<Value> receiver,
                          v8::Isolate* isolate) {
  TestSignatureLooped(operation, receiver, isolate);
  TestSignatureOptimized(operation, receiver, isolate);
}

THREADED_TEST(ReceiverSignature) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  // Setup templates.
  v8::Local<v8::FunctionTemplate> fun = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::Signature> sig = v8::Signature::New(isolate, fun);
  v8::Local<v8::FunctionTemplate> callback_sig = v8::FunctionTemplate::New(
      isolate, IncrementingSignatureCallback, Local<Value>(), sig);
  v8::Local<v8::FunctionTemplate> callback =
      v8::FunctionTemplate::New(isolate, IncrementingSignatureCallback);
  v8::Local<v8::FunctionTemplate> sub_fun = v8::FunctionTemplate::New(isolate);
  sub_fun->Inherit(fun);
  v8::Local<v8::FunctionTemplate> direct_sub_fun =
      v8::FunctionTemplate::New(isolate);
  direct_sub_fun->Inherit(fun);
  v8::Local<v8::FunctionTemplate> unrel_fun =
      v8::FunctionTemplate::New(isolate);
  // Install properties.
  v8::Local<v8::ObjectTemplate> fun_proto = fun->PrototypeTemplate();
  fun_proto->Set(v8_str("prop_sig"), callback_sig);
  fun_proto->Set(v8_str("prop"), callback);
  fun_proto->SetAccessorProperty(
      v8_str("accessor_sig"), callback_sig, callback_sig);
  fun_proto->SetAccessorProperty(v8_str("accessor"), callback, callback);
  // Instantiate templates.
  Local<Value> fun_instance =
      fun->InstanceTemplate()->NewInstance(env.local()).ToLocalChecked();
  Local<Value> sub_fun_instance =
      sub_fun->InstanceTemplate()->NewInstance(env.local()).ToLocalChecked();
  // Instance template with properties.
  v8::Local<v8::ObjectTemplate> direct_instance_templ =
      direct_sub_fun->InstanceTemplate();
  direct_instance_templ->Set(v8_str("prop_sig"), callback_sig);
  direct_instance_templ->Set(v8_str("prop"), callback);
  direct_instance_templ->SetAccessorProperty(v8_str("accessor_sig"),
                                             callback_sig, callback_sig);
  direct_instance_templ->SetAccessorProperty(v8_str("accessor"), callback,
                                             callback);
  Local<Value> direct_instance =
      direct_instance_templ->NewInstance(env.local()).ToLocalChecked();
  // Setup global variables.
  CHECK(env->Global()
            ->Set(env.local(), v8_str("Fun"),
                  fun->GetFunction(env.local()).ToLocalChecked())
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("UnrelFun"),
                  unrel_fun->GetFunction(env.local()).ToLocalChecked())
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("fun_instance"), fun_instance)
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("sub_fun_instance"), sub_fun_instance)
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("direct_instance"), direct_instance)
            .FromJust());
  CompileRun(
      "var accessor_sig_key = 'accessor_sig';"
      "var accessor_key = 'accessor';"
      "var prop_sig_key = 'prop_sig';"
      "var prop_key = 'prop';"
      ""
      "function copy_props(obj) {"
      "  var keys = [accessor_sig_key, accessor_key, prop_sig_key, prop_key];"
      "  var source = Fun.prototype;"
      "  for (var i in keys) {"
      "    var key = keys[i];"
      "    var desc = Object.getOwnPropertyDescriptor(source, key);"
      "    Object.defineProperty(obj, key, desc);"
      "  }"
      "}"
      ""
      "var plain = {};"
      "copy_props(plain);"
      "var unrelated = new UnrelFun();"
      "copy_props(unrelated);"
      "var inherited = { __proto__: fun_instance };"
      "var inherited_direct = { __proto__: direct_instance };");
  // Test with and without ICs
  const char* test_objects[] = {
      "fun_instance", "sub_fun_instance", "direct_instance", "plain",
      "unrelated",    "inherited",        "inherited_direct"};
  unsigned bad_signature_start_offset = 3;
  for (unsigned i = 0; i < arraysize(test_objects); i++) {
    i::ScopedVector<char> source(200);
    i::SNPrintF(
        source, "var test_object = %s; test_object", test_objects[i]);
    Local<Value> test_object = CompileRun(source.begin());
    TestSignature("test_object.prop();", test_object, isolate);
    TestSignature("test_object.accessor;", test_object, isolate);
    TestSignature("test_object[accessor_key];", test_object, isolate);
    TestSignature("test_object.accessor = 1;", test_object, isolate);
    TestSignature("test_object[accessor_key] = 1;", test_object, isolate);
    if (i >= bad_signature_start_offset) test_object = Local<Value>();
    TestSignature("test_object.prop_sig();", test_object, isolate);
    TestSignature("test_object.accessor_sig;", test_object, isolate);
    TestSignature("test_object[accessor_sig_key];", test_object, isolate);
    TestSignature("test_object.accessor_sig = 1;", test_object, isolate);
    TestSignature("test_object[accessor_sig_key] = 1;", test_object, isolate);
  }
}


THREADED_TEST(HulIgennem) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Primitive> undef = v8::Undefined(isolate);
  Local<String> undef_str = undef->ToString(env.local()).ToLocalChecked();
  char* value = i::NewArray<char>(undef_str->Utf8Length(isolate) + 1);
  undef_str->WriteUtf8(isolate, value);
  CHECK_EQ(0, strcmp(value, "undefined"));
  i::DeleteArray(value);
}


THREADED_TEST(Access) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<v8::Object> obj = v8::Object::New(isolate);
  Local<Value> foo_before =
      obj->Get(env.local(), v8_str("foo")).ToLocalChecked();
  CHECK(foo_before->IsUndefined());
  Local<String> bar_str = v8_str("bar");
  CHECK(obj->Set(env.local(), v8_str("foo"), bar_str).FromJust());
  Local<Value> foo_after =
      obj->Get(env.local(), v8_str("foo")).ToLocalChecked();
  CHECK(!foo_after->IsUndefined());
  CHECK(foo_after->IsString());
  CHECK(bar_str->Equals(env.local(), foo_after).FromJust());

  CHECK(obj->Set(env.local(), v8_str("foo"), bar_str).ToChecked());
  bool result;
  CHECK(obj->Set(env.local(), v8_str("foo"), bar_str).To(&result));
  CHECK(result);
}


THREADED_TEST(AccessElement) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  Local<Value> before = obj->Get(env.local(), 1).ToLocalChecked();
  CHECK(before->IsUndefined());
  Local<String> bar_str = v8_str("bar");
  CHECK(obj->Set(env.local(), 1, bar_str).FromJust());
  Local<Value> after = obj->Get(env.local(), 1).ToLocalChecked();
  CHECK(!after->IsUndefined());
  CHECK(after->IsString());
  CHECK(bar_str->Equals(env.local(), after).FromJust());

  Local<v8::Array> value = CompileRun("[\"a\", \"b\"]").As<v8::Array>();
  CHECK(v8_str("a")
            ->Equals(env.local(), value->Get(env.local(), 0).ToLocalChecked())
            .FromJust());
  CHECK(v8_str("b")
            ->Equals(env.local(), value->Get(env.local(), 1).ToLocalChecked())
            .FromJust());
}


THREADED_TEST(Script) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  const char* source = "1 + 2 + 3";
  Local<Script> script = v8_compile(source);
  CHECK_EQ(6, v8_run_int32value(script));
}


class TestResource: public String::ExternalStringResource {
 public:
  explicit TestResource(uint16_t* data, int* counter = nullptr,
                        bool owning_data = true)
      : data_(data), length_(0), counter_(counter), owning_data_(owning_data) {
    while (data[length_]) ++length_;
  }

  ~TestResource() override {
    if (owning_data_) i::DeleteArray(data_);
    if (counter_ != nullptr) ++*counter_;
  }

  const uint16_t* data() const override { return data_; }

  size_t length() const override { return length_; }

 private:
  uint16_t* data_;
  size_t length_;
  int* counter_;
  bool owning_data_;
};


class TestOneByteResource : public String::ExternalOneByteStringResource {
 public:
  explicit TestOneByteResource(const char* data, int* counter = nullptr,
                               size_t offset = 0)
      : orig_data_(data),
        data_(data + offset),
        length_(strlen(data) - offset),
        counter_(counter) {}

  ~TestOneByteResource() override {
    i::DeleteArray(orig_data_);
    if (counter_ != nullptr) ++*counter_;
  }

  const char* data() const override { return data_; }

  size_t length() const override { return length_; }

 private:
  const char* orig_data_;
  const char* data_;
  size_t length_;
  int* counter_;
};


THREADED_TEST(ScriptUsingStringResource) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  uint16_t* two_byte_source = AsciiToTwoByteString(c_source);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    TestResource* resource = new TestResource(two_byte_source, &dispose_count);
    Local<String> source =
        String::NewExternalTwoByte(env->GetIsolate(), resource)
            .ToLocalChecked();
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CHECK(source->IsExternal());
    CHECK_EQ(resource,
             static_cast<TestResource*>(source->GetExternalStringResource()));
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::TWO_BYTE_ENCODING, encoding);
    CcTest::CollectAllGarbage();
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllAvailableGarbage();
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptUsingOneByteStringResource) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    TestOneByteResource* resource =
        new TestOneByteResource(i::StrDup(c_source), &dispose_count);
    Local<String> source =
        String::NewExternalOneByte(env->GetIsolate(), resource)
            .ToLocalChecked();
    CHECK(source->IsExternalOneByte());
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalOneByteStringResource());
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CcTest::CollectAllGarbage();
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllAvailableGarbage();
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptMakingExternalString) {
  int dispose_count = 0;
  uint16_t* two_byte_source = AsciiToTwoByteString("1 + 2 * 3");
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =
        String::NewFromTwoByte(env->GetIsolate(), two_byte_source,
                               v8::NewStringType::kNormal)
            .ToLocalChecked();
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
    CHECK(!source->IsExternal());
    CHECK(!source->IsExternalOneByte());
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK(!source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
    bool success = source->MakeExternal(new TestResource(two_byte_source,
                                                         &dispose_count));
    CHECK(success);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CcTest::CollectAllGarbage();
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllGarbage();
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptMakingExternalOneByteString) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source = v8_str(c_source);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
    bool success = source->MakeExternal(
        new TestOneByteResource(i::StrDup(c_source), &dispose_count));
    CHECK(success);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CcTest::CollectAllGarbage();
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllGarbage();
  CHECK_EQ(1, dispose_count);
}


TEST(MakingExternalStringConditions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  // Free some space in the new space so that we can check freshness.
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::NEW_SPACE);

  uint16_t* two_byte_string = AsciiToTwoByteString("s1");
  Local<String> tiny_local_string =
      String::NewFromTwoByte(env->GetIsolate(), two_byte_string,
                             v8::NewStringType::kNormal)
          .ToLocalChecked();
  i::DeleteArray(two_byte_string);

  two_byte_string = AsciiToTwoByteString("s1234");
  Local<String> local_string =
      String::NewFromTwoByte(env->GetIsolate(), two_byte_string,
                             v8::NewStringType::kNormal)
          .ToLocalChecked();
  i::DeleteArray(two_byte_string);

  // We should refuse to externalize new space strings.
  CHECK(!local_string->CanMakeExternal());
  // Trigger GCs so that the newly allocated string moves to old gen.
  CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
  // Old space strings should be accepted.
  CHECK(local_string->CanMakeExternal());

  // Tiny strings are not in-place externalizable when pointer compression is
  // enabled.
  CHECK_EQ(i::kTaggedSize == i::kSystemPointerSize,
           tiny_local_string->CanMakeExternal());
}


TEST(MakingExternalOneByteStringConditions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  // Free some space in the new space so that we can check freshness.
  CcTest::CollectGarbage(i::NEW_SPACE);
  CcTest::CollectGarbage(i::NEW_SPACE);

  Local<String> tiny_local_string = v8_str("s");
  Local<String> local_string = v8_str("s1234");
  // We should refuse to externalize new space strings.
  CHECK(!local_string->CanMakeExternal());
  // Trigger GCs so that the newly allocated string moves to old gen.
  CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
  // Old space strings should be accepted.
  CHECK(local_string->CanMakeExternal());

  // Tiny strings are not in-place externalizable when pointer compression is
  // enabled.
  CHECK_EQ(i::kTaggedSize == i::kSystemPointerSize,
           tiny_local_string->CanMakeExternal());
}


TEST(MakingExternalUnalignedOneByteString) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CompileRun("function cons(a, b) { return a + b; }"
             "function slice(a) { return a.substring(1); }");
  // Create a cons string that will land in old pointer space.
  Local<String> cons = Local<String>::Cast(CompileRun(
      "cons('abcdefghijklm', 'nopqrstuvwxyz');"));
  // Create a sliced string that will land in old pointer space.
  Local<String> slice = Local<String>::Cast(CompileRun(
      "slice('abcdefghijklmnopqrstuvwxyz');"));

  // Trigger GCs so that the newly allocated string moves to old gen.
  i::heap::SimulateFullSpace(CcTest::heap()->old_space());
  CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now

  // Turn into external string with unaligned resource data.
  const char* c_cons = "_abcdefghijklmnopqrstuvwxyz";
  bool success = cons->MakeExternal(
      new TestOneByteResource(i::StrDup(c_cons), nullptr, 1));
  CHECK(success);
  const char* c_slice = "_bcdefghijklmnopqrstuvwxyz";
  success = slice->MakeExternal(
      new TestOneByteResource(i::StrDup(c_slice), nullptr, 1));
  CHECK(success);

  // Trigger GCs and force evacuation.
  CcTest::CollectAllGarbage();
  CcTest::heap()->CollectAllGarbage(i::Heap::kReduceMemoryFootprintMask,
                                    i::GarbageCollectionReason::kTesting);
}

THREADED_TEST(UsingExternalString) {
  i::Factory* factory = CcTest::i_isolate()->factory();
  {
    v8::HandleScope scope(CcTest::isolate());
    uint16_t* two_byte_string = AsciiToTwoByteString("test string");
    Local<String> string =
        String::NewExternalTwoByte(CcTest::isolate(),
                                   new TestResource(two_byte_string))
            .ToLocalChecked();
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
    i::Handle<i::String> isymbol =
        factory->InternalizeString(istring);
    CHECK(isymbol->IsInternalizedString());
  }
  CcTest::CollectAllGarbage();
  CcTest::CollectAllGarbage();
}


THREADED_TEST(UsingExternalOneByteString) {
  i::Factory* factory = CcTest::i_isolate()->factory();
  {
    v8::HandleScope scope(CcTest::isolate());
    const char* one_byte_string = "test string";
    Local<String> string =
        String::NewExternalOneByte(
            CcTest::isolate(),
            new TestOneByteResource(i::StrDup(one_byte_string)))
            .ToLocalChecked();
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::CollectGarbage(i::NEW_SPACE);  // in old gen now
    i::Handle<i::String> isymbol =
        factory->InternalizeString(istring);
    CHECK(isymbol->IsInternalizedString());
  }
  CcTest::CollectAllGarbage();
  CcTest::CollectAllGarbage();
}


class RandomLengthResource : public v8::String::ExternalStringResource {
 public:
  explicit RandomLengthResource(int length) : length_(length) {}
  const uint16_t* data() const override { return string_; }
  size_t length() const override { return length_; }

 private:
  uint16_t string_[10];
  int length_;
};


class RandomLengthOneByteResource
    : public v8::String::ExternalOneByteStringResource {
 public:
  explicit RandomLengthOneByteResource(int length) : length_(length) {}
  const char* data() const override { return string_; }
  size_t length() const override { return length_; }

 private:
  char string_[10];
  int length_;
};


THREADED_TEST(NewExternalForVeryLongString) {
  auto isolate = CcTest::isolate();
  {
    v8::HandleScope scope(isolate);
    v8::TryCatch try_catch(isolate);
    RandomLengthOneByteResource r(1 << 30);
    v8::MaybeLocal<v8::String> maybe_str =
        v8::String::NewExternalOneByte(isolate, &r);
    CHECK(maybe_str.IsEmpty());
    CHECK(!try_catch.HasCaught());
  }

  {
    v8::HandleScope scope(isolate);
    v8::TryCatch try_catch(isolate);
    RandomLengthResource r(1 << 30);
    v8::MaybeLocal<v8::String> maybe_str =
        v8::String::NewExternalTwoByte(isolate, &r);
    CHECK(maybe_str.IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
}

TEST(ScavengeExternalString) {
  ManualGCScope manual_gc_scope;
  i::FLAG_stress_compaction = false;
  i::FLAG_gc_global = false;
  int dispose_count = 0;
  bool in_young_generation = false;
  {
    v8::HandleScope scope(CcTest::isolate());
    uint16_t* two_byte_string = AsciiToTwoByteString("test string");
    Local<String> string =
        String::NewExternalTwoByte(
            CcTest::isolate(),
            new TestResource(two_byte_string, &dispose_count))
            .ToLocalChecked();
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    CcTest::CollectGarbage(i::NEW_SPACE);
    in_young_generation = i::Heap::InYoungGeneration(*istring);
    CHECK_IMPLIES(!in_young_generation,
                  CcTest::heap()->old_space()->Contains(*istring));
    CHECK_EQ(0, dispose_count);
  }
  CcTest::CollectGarbage(in_young_generation ? i::NEW_SPACE : i::OLD_SPACE);
  CHECK_EQ(1, dispose_count);
}

TEST(ScavengeExternalOneByteString) {
  ManualGCScope manual_gc_scope;
  i::FLAG_stress_compaction = false;
  i::FLAG_gc_global = false;
  int dispose_count = 0;
  bool in_young_generation = false;
  {
    v8::HandleScope scope(CcTest::isolate());
    const char* one_byte_string = "test string";
    Local<String> string =
        String::NewExternalOneByte(
            CcTest::isolate(),
            new TestOneByteResource(i::StrDup(one_byte_string), &dispose_count))
            .ToLocalChecked();
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    CcTest::CollectGarbage(i::NEW_SPACE);
    in_young_generation = i::Heap::InYoungGeneration(*istring);
    CHECK_IMPLIES(!in_young_generation,
                  CcTest::heap()->old_space()->Contains(*istring));
    CHECK_EQ(0, dispose_count);
  }
  CcTest::CollectGarbage(in_young_generation ? i::NEW_SPACE : i::OLD_SPACE);
  CHECK_EQ(1, dispose_count);
}


class TestOneByteResourceWithDisposeControl : public TestOneByteResource {
 public:
  // Only used by non-threaded tests, so it can use static fields.
  static int dispose_calls;
  static int dispose_count;

  TestOneByteResourceWithDisposeControl(const char* data, bool dispose)
      : TestOneByteResource(data, &dispose_count), dispose_(dispose) {}

  void Dispose() override {
    ++dispose_calls;
    if (dispose_) delete this;
  }
 private:
  bool dispose_;
};


int TestOneByteResourceWithDisposeControl::dispose_count = 0;
int TestOneByteResourceWithDisposeControl::dispose_calls = 0;


TEST(ExternalStringWithDisposeHandling) {
  const char* c_source = "1 + 2 * 3";

  // Use a stack allocated external string resource allocated object.
  TestOneByteResourceWithDisposeControl::dispose_count = 0;
  TestOneByteResourceWithDisposeControl::dispose_calls = 0;
  TestOneByteResourceWithDisposeControl res_stack(i::StrDup(c_source), false);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =
        String::NewExternalOneByte(env->GetIsolate(), &res_stack)
            .ToLocalChecked();
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CcTest::CollectAllAvailableGarbage();
    CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllAvailableGarbage();
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_calls);
  CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);

  // Use a heap allocated external string resource allocated object.
  TestOneByteResourceWithDisposeControl::dispose_count = 0;
  TestOneByteResourceWithDisposeControl::dispose_calls = 0;
  TestOneByteResource* res_heap =
      new TestOneByteResourceWithDisposeControl(i::StrDup(c_source), true);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =
        String::NewExternalOneByte(env->GetIsolate(), res_heap)
            .ToLocalChecked();
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value(env.local()).FromJust());
    CcTest::CollectAllAvailableGarbage();
    CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllAvailableGarbage();
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_calls);
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_count);
}


THREADED_TEST(StringConcat) {
  {
    LocalContext env;
    v8::Isolate* isolate = env->GetIsolate();
    v8::HandleScope scope(isolate);
    const char* one_byte_string_1 = "function a_times_t";
    const char* two_byte_string_1 = "wo_plus_b(a, b) {return ";
    const char* one_byte_extern_1 = "a * 2 + b;} a_times_two_plus_b(4, 8) + ";
    const char* two_byte_extern_1 = "a_times_two_plus_b(4, 8) + ";
    const char* one_byte_string_2 = "a_times_two_plus_b(4, 8) + ";
    const char* two_byte_string_2 = "a_times_two_plus_b(4, 8) + ";
    const char* two_byte_extern_2 = "a_times_two_plus_b(1, 2);";
    Local<String> left = v8_str(one_byte_string_1);

    uint16_t* two_byte_source = AsciiToTwoByteString(two_byte_string_1);
    Local<String> right =
        String::NewFromTwoByte(env->GetIsolate(), two_byte_source,
                               v8::NewStringType::kNormal)
            .ToLocalChecked();
    i::DeleteArray(two_byte_source);

    Local<String> source = String::Concat(isolate, left, right);
    right = String::NewExternalOneByte(
                env->GetIsolate(),
                new TestOneByteResource(i::StrDup(one_byte_extern_1)))
                .ToLocalChecked();
    source = String::Concat(isolate, source, right);
    right = String::NewExternalTwoByte(
                env->GetIsolate(),
                new TestResource(AsciiToTwoByteString(two_byte_extern_1)))
                .ToLocalChecked();
    source = String::Concat(isolate, source, right);
    right = v8_str(one_byte_string_2);
    source = String::Concat(isolate, source, right);

    two_byte_source = AsciiToTwoByteString(two_byte_string_2);
    right = String::NewFromTwoByte(env->GetIsolate(), two_byte_source,
                                   v8::NewStringType::kNormal)
                .ToLocalChecked();
    i::DeleteArray(two_byte_source);

    source = String::Concat(isolate, source, right);
    right = String::NewExternalTwoByte(
                env->GetIsolate(),
                new TestResource(AsciiToTwoByteString(two_byte_extern_2)))
                .ToLocalChecked();
    source = String::Concat(isolate, source, right);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run(env.local()).ToLocalChecked();
    CHECK(value->IsNumber());
    CHECK_EQ(68, value->Int32Value(env.local()).FromJust());
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllGarbage();
  CcTest::CollectAllGarbage();
}


THREADED_TEST(GlobalProperties) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<v8::Object> global = env->Global();
  CHECK(global->Set(env.local(), v8_str("pi"), v8_num(3.1415926)).FromJust());
  Local<Value> pi = global->Get(env.local(), v8_str("pi")).ToLocalChecked();
  CHECK_EQ(3.1415926, pi->NumberValue(env.local()).FromJust());
}


static void handle_callback_impl(const v8::FunctionCallbackInfo<Value>& info,
                                 i::Address callback) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, callback);
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(v8_num(102));
}


static void handle_callback(const v8::FunctionCallbackInfo<Value>& info) {
  return handle_callback_impl(info, FUNCTION_ADDR(handle_callback));
}


static void handle_callback_2(const v8::FunctionCallbackInfo<Value>& info) {
  return handle_callback_impl(info, FUNCTION_ADDR(handle_callback_2));
}

static void construct_callback(
    const v8::FunctionCallbackInfo<Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(construct_callback));
  CHECK(
      info.This()
          ->Set(info.GetIsolate()->GetCurrentContext(), v8_str("x"), v8_num(1))
          .FromJust());
  CHECK(
      info.This()
          ->Set(info.GetIsolate()->GetCurrentContext(), v8_str("y"), v8_num(2))
          .FromJust());
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(info.This());
}


static void Return239Callback(
    Local<String> name, const v8::PropertyCallbackInfo<Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(Return239Callback));
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(v8_num(239));
}


template<typename Handler>
static void TestFunctionTemplateInitializer(Handler handler,
                                            Handler handler_2) {
  // Test constructor calls.
  {
    LocalContext env;
    v8::Isolate* isolate = env->GetIsolate();
    v8::HandleScope scope(isolate);

    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handler);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
    Local<Script> script = v8_compile("obj()");
    for (int i = 0; i < 30; i++) {
      CHECK_EQ(102, v8_run_int32value(script));
    }
  }
  // Use SetCallHandler to initialize a function template, should work like
  // the previous one.
  {
    LocalContext env;
    v8::Isolate* isolate = env->GetIsolate();
    v8::HandleScope scope(isolate);

    Local<v8::FunctionTemplate> fun_templ = v8::FunctionTemplate::New(isolate);
    fun_templ->SetCallHandler(handler_2);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
    Local<Script> script = v8_compile("obj()");
    for (int i = 0; i < 30; i++) {
      CHECK_EQ(102, v8_run_int32value(script));
    }
  }
}


template<typename Constructor, typename Accessor>
static void TestFunctionTemplateAccessor(Constructor constructor,
                                         Accessor accessor) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), constructor);
  fun_templ->SetClassName(v8_str("funky"));
  fun_templ->InstanceTemplate()->SetAccessor(v8_str("m"), accessor);
  Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
  Local<Value> result =
      v8_compile("(new obj()).toString()")->Run(env.local()).ToLocalChecked();
  CHECK(v8_str("[object funky]")->Equals(env.local(), result).FromJust());
  CompileRun("var obj_instance = new obj();");
  Local<Script> script;
  script = v8_compile("obj_instance.x");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(1, v8_run_int32value(script));
  }
  script = v8_compile("obj_instance.m");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(239, v8_run_int32value(script));
  }
}


THREADED_PROFILED_TEST(FunctionTemplate) {
  TestFunctionTemplateInitializer(handle_callback, handle_callback_2);
  TestFunctionTemplateAccessor(construct_callback, Return239Callback);
}

static void FunctionCallbackForProxyTest(
    const v8::FunctionCallbackInfo<Value>& info) {
  info.GetReturnValue().Set(info.This());
}

THREADED_TEST(FunctionTemplateWithProxy) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::FunctionTemplate> function_template =
      v8::FunctionTemplate::New(isolate, FunctionCallbackForProxyTest);
  v8::Local<v8::Function> function =
      function_template->GetFunction(env.local()).ToLocalChecked();
  CHECK((*env)->Global()->Set(env.local(), v8_str("f"), function).FromJust());
  v8::Local<v8::Value> proxy =
      CompileRun("var proxy = new Proxy({}, {}); proxy");
  CHECK(proxy->IsProxy());

  v8::Local<v8::Value> result = CompileRun("f(proxy)");
  CHECK(result->Equals(env.local(), (*env)->Global()).FromJust());

  result = CompileRun("f.call(proxy)");
  CHECK(result->Equals(env.local(), proxy).FromJust());

  result = CompileRun("Reflect.apply(f, proxy, [1])");
  CHECK(result->Equals(env.local(), proxy).FromJust());
}

static void SimpleCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(SimpleCallback));
  info.GetReturnValue().Set(v8_num(51423 + info.Length()));
}


template<typename Callback>
static void TestSimpleCallback(Callback callback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->Set(isolate, "callback",
                       v8::FunctionTemplate::New(isolate, callback));
  v8::Local<v8::Object> object =
      object_template->NewInstance(env.local()).ToLocalChecked();
  CHECK((*env)
            ->Global()
            ->Set(env.local(), v8_str("callback_object"), object)
            .FromJust());
  v8::Local<v8::Script> script;
  script = v8_compile("callback_object.callback(17)");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(51424, v8_run_int32value(script));
  }
  script = v8_compile("callback_object.callback(17, 24)");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(51425, v8_run_int32value(script));
  }
}


THREADED_PROFILED_TEST(SimpleCallback) {
  TestSimpleCallback(SimpleCallback);
}


template<typename T>
void FastReturnValueCallback(const v8::FunctionCallbackInfo<v8::Value>& info);

// constant return values
static int32_t fast_return_value_int32 = 471;
static uint32_t fast_return_value_uint32 = 571;
static const double kFastReturnValueDouble = 2.7;
// variable return values
static bool fast_return_value_bool = false;
enum ReturnValueOddball {
  kNullReturnValue,
  kUndefinedReturnValue,
  kEmptyStringReturnValue
};
static ReturnValueOddball fast_return_value_void;
static bool fast_return_value_object_is_empty = false;

// Helper function to avoid compiler error: insufficient contextual information
// to determine type when applying FUNCTION_ADDR to a template function.
static i::Address address_of(v8::FunctionCallback callback) {
  return FUNCTION_ADDR(callback);
}

template<>
void FastReturnValueCallback<int32_t>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<int32_t>));
  info.GetReturnValue().Set(fast_return_value_int32);
}

template<>
void FastReturnValueCallback<uint32_t>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<uint32_t>));
  info.GetReturnValue().Set(fast_return_value_uint32);
}

template<>
void FastReturnValueCallback<double>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<double>));
  info.GetReturnValue().Set(kFastReturnValueDouble);
}

template<>
void FastReturnValueCallback<bool>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<bool>));
  info.GetReturnValue().Set(fast_return_value_bool);
}

template<>
void FastReturnValueCallback<void>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<void>));
  switch (fast_return_value_void) {
    case kNullReturnValue:
      info.GetReturnValue().SetNull();
      break;
    case kUndefinedReturnValue:
      info.GetReturnValue().SetUndefined();
      break;
    case kEmptyStringReturnValue:
      info.GetReturnValue().SetEmptyString();
      break;
  }
}

template<>
void FastReturnValueCallback<Object>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Object> object;
  if (!fast_return_value_object_is_empty) {
    object = Object::New(info.GetIsolate());
  }
  info.GetReturnValue().Set(object);
}

template <typename T>
Local<Value> TestFastReturnValues() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::EscapableHandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  v8::FunctionCallback callback = &FastReturnValueCallback<T>;
  object_template->Set(isolate, "callback",
                       v8::FunctionTemplate::New(isolate, callback));
  v8::Local<v8::Object> object =
      object_template->NewInstance(env.local()).ToLocalChecked();
  CHECK((*env)
            ->Global()
            ->Set(env.local(), v8_str("callback_object"), object)
            .FromJust());
  return scope.Escape(CompileRun("callback_object.callback()"));
}


THREADED_PROFILED_TEST(FastReturnValues) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> value;
  // check int32_t and uint32_t
  int32_t int_values[] = {
      0, 234, -723,
      i::Smi::kMinValue, i::Smi::kMaxValue
  };
  for (size_t i = 0; i < arraysize(int_values); i++) {
    for (int modifier = -1; modifier <= 1; modifier++) {
      int int_value = v8::base::AddWithWraparound(int_values[i], modifier);
      // check int32_t
      fast_return_value_int32 = int_value;
      value = TestFastReturnValues<int32_t>();
      CHECK(value->IsInt32());
      CHECK_EQ(fast_return_value_int32,
               value->Int32Value(env.local()).FromJust());
      // check uint32_t
      fast_return_value_uint32 = static_cast<uint32_t>(int_value);
      value = TestFastReturnValues<uint32_t>();
      CHECK(value->IsUint32());
      CHECK_EQ(fast_return_value_uint32,
               value->Uint32Value(env.local()).FromJust());
    }
  }
  // check double
  value = TestFastReturnValues<double>();
  CHECK(value->IsNumber());
  CHECK_EQ(kFastReturnValueDouble,
           value->ToNumber(env.local()).ToLocalChecked()->Value());
  // check bool values
  for (int i = 0; i < 2; i++) {
    fast_return_value_bool = i == 0;
    value = TestFastReturnValues<bool>();
    CHECK(value->IsBoolean());
    CHECK_EQ(fast_return_value_bool, value->BooleanValue(isolate));
  }
  // check oddballs
  ReturnValueOddball oddballs[] = {
      kNullReturnValue,
      kUndefinedReturnValue,
      kEmptyStringReturnValue
  };
  for (size_t i = 0; i < arraysize(oddballs); i++) {
    fast_return_value_void = oddballs[i];
    value = TestFastReturnValues<void>();
    switch (fast_return_value_void) {
      case kNullReturnValue:
        CHECK(value->IsNull());
        break;
      case kUndefinedReturnValue:
        CHECK(value->IsUndefined());
        break;
      case kEmptyStringReturnValue:
        CHECK(value->IsString());
        CHECK_EQ(0, v8::String::Cast(*value)->Length());
        break;
    }
  }
  // check handles
  fast_return_value_object_is_empty = false;
  value = TestFastReturnValues<Object>();
  CHECK(value->IsObject());
  fast_return_value_object_is_empty = true;
  value = TestFastReturnValues<Object>();
  CHECK(value->IsUndefined());
}


THREADED_TEST(FunctionTemplateSetLength) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  {
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handle_callback, Local<v8::Value>(),
                                  Local<v8::Signature>(), 23);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(23, v8_run_int32value(script));
  }
  {
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handle_callback);
    fun_templ->SetLength(22);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(22, v8_run_int32value(script));
  }
  {
    // Without setting length it defaults to 0.
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handle_callback);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), fun).FromJust());
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(0, v8_run_int32value(script));
  }
}


static void* expected_ptr;
static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  void* ptr = v8::External::Cast(*args.Data())->Value();
  CHECK_EQ(expected_ptr, ptr);
  args.GetReturnValue().Set(true);
}


static void TestExternalPointerWrapping() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Value> data = v8::External::New(isolate, expected_ptr);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  CHECK(obj->Set(env.local(), v8_str("func"),
                 v8::FunctionTemplate::New(isolate, callback, data)
                     ->GetFunction(env.local())
                     .ToLocalChecked())
            .FromJust());
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), obj).FromJust());

  CHECK(CompileRun("function foo() {\n"
                   "  for (var i = 0; i < 13; i++) obj.func();\n"
                   "}\n"
                   "foo(), true")
            ->BooleanValue(isolate));
}


THREADED_TEST(ExternalWrap) {
  // Check heap allocated object.
  int* ptr = new int;
  expected_ptr = ptr;
  TestExternalPointerWrapping();
  delete ptr;

  // Check stack allocated object.
  int foo;
  expected_ptr = &foo;
  TestExternalPointerWrapping();

  // Check not aligned addresses.
  const int n = 100;
  char* s = new char[n];
  for (int i = 0; i < n; i++) {
    expected_ptr = s + i;
    TestExternalPointerWrapping();
  }

  delete[] s;

  // Check several invalid addresses.
  expected_ptr = reinterpret_cast<void*>(1);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xDEADBEEF);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xDEADBEEF + 1);
  TestExternalPointerWrapping();

#if defined(V8_HOST_ARCH_X64)
  // Check a value with a leading 1 bit in x64 Smi encoding.
  expected_ptr = reinterpret_cast<void*>(0x400000000);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xDEADBEEFDEADBEEF);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xDEADBEEFDEADBEEF + 1);
  TestExternalPointerWrapping();
#endif
}


THREADED_TEST(FindInstanceInPrototypeChain) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> base = v8::FunctionTemplate::New(isolate);
  Local<v8::FunctionTemplate> derived = v8::FunctionTemplate::New(isolate);
  Local<v8::FunctionTemplate> other = v8::FunctionTemplate::New(isolate);
  derived->Inherit(base);

  Local<v8::Function> base_function =
      base->GetFunction(env.local()).ToLocalChecked();
  Local<v8::Function> derived_function =
      derived->GetFunction(env.local()).ToLocalChecked();
  Local<v8::Function> other_function =
      other->GetFunction(env.local()).ToLocalChecked();

  Local<v8::Object> base_instance =
      base_function->NewInstance(env.local()).ToLocalChecked();
  Local<v8::Object> derived_instance =
      derived_function->NewInstance(env.local()).ToLocalChecked();
  Local<v8::Object> derived_instance2 =
      derived_function->NewInstance(env.local()).ToLocalChecked();
  Local<v8::Object> other_instance =
      other_function->NewInstance(env.local()).ToLocalChecked();
  CHECK(
      derived_instance2->Set(env.local(), v8_str("__proto__"), derived_instance)
          .FromJust());
  CHECK(other_instance->Set(env.local(), v8_str("__proto__"), derived_instance2)
            .FromJust());

  // base_instance is only an instance of base.
  CHECK(base_instance->Equals(env.local(),
                              base_instance->FindInstanceInPrototypeChain(base))
            .FromJust());
  CHECK(base_instance->FindInstanceInPrototypeChain(derived).IsEmpty());
  CHECK(base_instance->FindInstanceInPrototypeChain(other).IsEmpty());

  // derived_instance is an instance of base and derived.
  CHECK(derived_instance->Equals(env.local(),
                                 derived_instance->FindInstanceInPrototypeChain(
                                     base))
            .FromJust());
  CHECK(derived_instance->Equals(env.local(),
                                 derived_instance->FindInstanceInPrototypeChain(
                                     derived))
            .FromJust());
  CHECK(derived_instance->FindInstanceInPrototypeChain(other).IsEmpty());

  // other_instance is an instance of other and its immediate
  // prototype derived_instance2 is an instance of base and derived.
  // Note, derived_instance is an instance of base and derived too,
  // but it comes after derived_instance2 in the prototype chain of
  // other_instance.
  CHECK(derived_instance2->Equals(
                             env.local(),
                             other_instance->FindInstanceInPrototypeChain(base))
            .FromJust());
  CHECK(derived_instance2->Equals(env.local(),
                                  other_instance->FindInstanceInPrototypeChain(
                                      derived))
            .FromJust());
  CHECK(other_instance->Equals(
                          env.local(),
                          other_instance->FindInstanceInPrototypeChain(other))
            .FromJust());
}


THREADED_TEST(TinyInteger) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  int32_t value = 239;
  Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigSmiInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  int32_t value = i::Smi::kMaxValue;
  // We cannot add one to a Smi::kMaxValue without wrapping.
  if (i::SmiValuesAre31Bits()) {
    CHECK(i::Smi::IsValid(value));
    CHECK(!i::Smi::IsValid(value + 1));

    Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
  }
}


THREADED_TEST(BigInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  // We cannot add one to a Smi::kMaxValue without wrapping.
  if (i::SmiValuesAre31Bits()) {
    // The casts allow this to compile, even if Smi::kMaxValue is 2^31-1.
    // The code will not be run in that case, due to the "if" guard.
    int32_t value =
        static_cast<int32_t>(static_cast<uint32_t>(i::Smi::kMaxValue) + 1);
    CHECK_GT(value, i::Smi::kMaxValue);
    CHECK(!i::Smi::IsValid(value));

    Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
  }
}


THREADED_TEST(TinyUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = 239;

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigUnsignedSmiInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = static_cast<uint32_t>(i::Smi::kMaxValue);
  CHECK(i::Smi::IsValid(value));
  CHECK(!i::Smi::IsValid(value + 1));

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = static_cast<uint32_t>(i::Smi::kMaxValue) + 1;
  CHECK(value > static_cast<uint32_t>(i::Smi::kMaxValue));
  CHECK(!i::Smi::IsValid(value));

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(OutOfSignedRangeUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t INT32_MAX_AS_UINT = (1U << 31) - 1;
  uint32_t value = INT32_MAX_AS_UINT + 1;
  CHECK(value > INT32_MAX_AS_UINT);  // No overflow.

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(IsNativeError) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> syntax_error = CompileRun(
      "var out = 0; try { eval(\"#\"); } catch(x) { out = x; } out; ");
  CHECK(syntax_error->IsNativeError());
  v8::Local<Value> not_error = CompileRun("{a:42}");
  CHECK(!not_error->IsNativeError());
  v8::Local<Value> not_object = CompileRun("42");
  CHECK(!not_object->IsNativeError());
}


THREADED_TEST(IsGeneratorFunctionOrObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CompileRun("function *gen() { yield 1; }\nfunction func() {}");
  v8::Local<Value> gen = CompileRun("gen");
  v8::Local<Value> genObj = CompileRun("gen()");
  v8::Local<Value> object = CompileRun("{a:42}");
  v8::Local<Value> func = CompileRun("func");

  CHECK(gen->IsGeneratorFunction());
  CHECK(gen->IsFunction());
  CHECK(!gen->IsGeneratorObject());

  CHECK(!genObj->IsGeneratorFunction());
  CHECK(!genObj->IsFunction());
  CHECK(genObj->IsGeneratorObject());

  CHECK(!object->IsGeneratorFunction());
  CHECK(!object->IsFunction());
  CHECK(!object->IsGeneratorObject());

  CHECK(!func->IsGeneratorFunction());
  CHECK(func->IsFunction());
  CHECK(!func->IsGeneratorObject());
}

THREADED_TEST(IsAsyncFunction) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  CompileRun("async function foo() {}");
  v8::Local<Value> foo = CompileRun("foo");

  CHECK(foo->IsAsyncFunction());
  CHECK(foo->IsFunction());
  CHECK(!foo->IsGeneratorFunction());
  CHECK(!foo->IsGeneratorObject());

  CompileRun("function bar() {}");
  v8::Local<Value> bar = CompileRun("bar");

  CHECK(!bar->IsAsyncFunction());
  CHECK(bar->IsFunction());
}

THREADED_TEST(ArgumentsObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> arguments_object =
      CompileRun("var out = 0; (function(){ out = arguments; })(1,2,3); out;");
  CHECK(arguments_object->IsArgumentsObject());
  v8::Local<Value> array = CompileRun("[1,2,3]");
  CHECK(!array->IsArgumentsObject());
  v8::Local<Value> object = CompileRun("{a:42}");
  CHECK(!object->IsArgumentsObject());
}


THREADED_TEST(IsMapOrSet) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> map = CompileRun("new Map()");
  v8::Local<Value> set = CompileRun("new Set()");
  v8::Local<Value> weak_map = CompileRun("new WeakMap()");
  v8::Local<Value> weak_set = CompileRun("new WeakSet()");
  CHECK(map->IsMap());
  CHECK(set->IsSet());
  CHECK(weak_map->IsWeakMap());
  CHECK(weak_set->IsWeakSet());

  CHECK(!map->IsSet());
  CHECK(!map->IsWeakMap());
  CHECK(!map->IsWeakSet());

  CHECK(!set->IsMap());
  CHECK(!set->IsWeakMap());
  CHECK(!set->IsWeakSet());

  CHECK(!weak_map->IsMap());
  CHECK(!weak_map->IsSet());
  CHECK(!weak_map->IsWeakSet());

  CHECK(!weak_set->IsMap());
  CHECK(!weak_set->IsSet());
  CHECK(!weak_set->IsWeakMap());

  v8::Local<Value> object = CompileRun("{a:42}");
  CHECK(!object->IsMap());
  CHECK(!object->IsSet());
  CHECK(!object->IsWeakMap());
  CHECK(!object->IsWeakSet());
}


THREADED_TEST(StringObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> boxed_string = CompileRun("new String(\"test\")");
  CHECK(boxed_string->IsStringObject());
  v8::Local<Value> unboxed_string = CompileRun("\"test\"");
  CHECK(!unboxed_string->IsStringObject());
  v8::Local<Value> boxed_not_string = CompileRun("new Number(42)");
  CHECK(!boxed_not_string->IsStringObject());
  v8::Local<Value> not_object = CompileRun("0");
  CHECK(!not_object->IsStringObject());
  v8::Local<v8::StringObject> as_boxed = boxed_string.As<v8::StringObject>();
  CHECK(!as_boxed.IsEmpty());
  Local<v8::String> the_string = as_boxed->ValueOf();
  CHECK(!the_string.IsEmpty());
  ExpectObject("\"test\"", the_string);
  v8::Local<v8::Value> new_boxed_string =
      v8::StringObject::New(CcTest::isolate(), the_string);
  CHECK(new_boxed_string->IsStringObject());
  as_boxed = new_boxed_string.As<v8::StringObject>();
  the_string = as_boxed->ValueOf();
  CHECK(!the_string.IsEmpty());
  ExpectObject("\"test\"", the_string);
}


TEST(StringObjectDelete) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<Value> boxed_string = CompileRun("new String(\"test\")");
  CHECK(boxed_string->IsStringObject());
  v8::Local<v8::Object> str_obj = boxed_string.As<v8::Object>();
  CHECK(!str_obj->Delete(context.local(), 2).FromJust());
  CHECK(!str_obj->Delete(context.local(), v8_num(2)).FromJust());
}


THREADED_TEST(NumberObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> boxed_number = CompileRun("new Number(42)");
  CHECK(boxed_number->IsNumberObject());
  v8::Local<Value> unboxed_number = CompileRun("42");
  CHECK(!unboxed_number->IsNumberObject());
  v8::Local<Value> boxed_not_number = CompileRun("new Boolean(false)");
  CHECK(!boxed_not_number->IsNumberObject());
  v8::Local<v8::NumberObject> as_boxed = boxed_number.As<v8::NumberObject>();
  CHECK(!as_boxed.IsEmpty());
  double the_number = as_boxed->ValueOf();
  CHECK_EQ(42.0, the_number);
  v8::Local<v8::Value> new_boxed_number =
      v8::NumberObject::New(env->GetIsolate(), 43);
  CHECK(new_boxed_number->IsNumberObject());
  as_boxed = new_boxed_number.As<v8::NumberObject>();
  the_number = as_boxed->ValueOf();
  CHECK_EQ(43.0, the_number);
}

THREADED_TEST(BigIntObject) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context(env.local());
  v8::Local<Value> boxed_bigint = CompileRun("new Object(42n)");
  CHECK(!boxed_bigint->IsBigInt());
  CHECK(boxed_bigint->IsBigIntObject());
  v8::Local<Value> unboxed_bigint = CompileRun("42n");
  CHECK(unboxed_bigint->IsBigInt());
  CHECK(!unboxed_bigint->IsBigIntObject());
  v8::Local<v8::BigIntObject> as_boxed = boxed_bigint.As<v8::BigIntObject>();
  CHECK(!as_boxed.IsEmpty());
  v8::Local<v8::BigInt> unpacked = as_boxed->ValueOf();
  CHECK(!unpacked.IsEmpty());
  v8::Local<v8::Value> new_boxed_bigint = v8::BigIntObject::New(isolate, 43);
  CHECK(new_boxed_bigint->IsBigIntObject());
  v8::Local<v8::Value> new_unboxed_bigint = v8::BigInt::New(isolate, 44);
  CHECK(new_unboxed_bigint->IsBigInt());

  // Test functionality inherited from v8::Value.
  CHECK(unboxed_bigint->BooleanValue(isolate));
  v8::Local<v8::String> string =
      unboxed_bigint->ToString(context).ToLocalChecked();
  CHECK_EQ(0, strcmp("42", *v8::String::Utf8Value(isolate, string)));

  // IntegerValue throws.
  CHECK(unboxed_bigint->IntegerValue(context).IsNothing());
}

THREADED_TEST(BooleanObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> boxed_boolean = CompileRun("new Boolean(true)");
  CHECK(boxed_boolean->IsBooleanObject());
  v8::Local<Value> unboxed_boolean = CompileRun("true");
  CHECK(!unboxed_boolean->IsBooleanObject());
  v8::Local<Value> boxed_not_boolean = CompileRun("new Number(42)");
  CHECK(!boxed_not_boolean->IsBooleanObject());
  v8::Local<v8::BooleanObject> as_boxed = boxed_boolean.As<v8::BooleanObject>();
  CHECK(!as_boxed.IsEmpty());
  bool the_boolean = as_boxed->ValueOf();
  CHECK(the_boolean);
  v8::Local<v8::Value> boxed_true =
      v8::BooleanObject::New(env->GetIsolate(), true);
  v8::Local<v8::Value> boxed_false =
      v8::BooleanObject::New(env->GetIsolate(), false);
  CHECK(boxed_true->IsBooleanObject());
  CHECK(boxed_false->IsBooleanObject());
  as_boxed = boxed_true.As<v8::BooleanObject>();
  CHECK(as_boxed->ValueOf());
  as_boxed = boxed_false.As<v8::BooleanObject>();
  CHECK(!as_boxed->ValueOf());
}


THREADED_TEST(PrimitiveAndWrappedBooleans) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<Value> primitive_false = Boolean::New(isolate, false);
  CHECK(primitive_false->IsBoolean());
  CHECK(!primitive_false->IsBooleanObject());
  CHECK(!primitive_false->BooleanValue(isolate));
  CHECK(!primitive_false->IsTrue());
  CHECK(primitive_false->IsFalse());

  Local<Value> false_value = BooleanObject::New(isolate, false);
  CHECK(!false_value->IsBoolean());
  CHECK(false_value->IsBooleanObject());
  CHECK(false_value->BooleanValue(isolate));
  CHECK(!false_value->IsTrue());
  CHECK(!false_value->IsFalse());

  Local<BooleanObject> false_boolean_object = false_value.As<BooleanObject>();
  CHECK(!false_boolean_object->IsBoolean());
  CHECK(false_boolean_object->IsBooleanObject());
  CHECK(false_boolean_object->BooleanValue(isolate));
  CHECK(!false_boolean_object->ValueOf());
  CHECK(!false_boolean_object->IsTrue());
  CHECK(!false_boolean_object->IsFalse());

  Local<Value> primitive_true = Boolean::New(isolate, true);
  CHECK(primitive_true->IsBoolean());
  CHECK(!primitive_true->IsBooleanObject());
  CHECK(primitive_true->BooleanValue(isolate));
  CHECK(primitive_true->IsTrue());
  CHECK(!primitive_true->IsFalse());

  Local<Value> true_value = BooleanObject::New(isolate, true);
  CHECK(!true_value->IsBoolean());
  CHECK(true_value->IsBooleanObject());
  CHECK(true_value->BooleanValue(isolate));
  CHECK(!true_value->IsTrue());
  CHECK(!true_value->IsFalse());

  Local<BooleanObject> true_boolean_object = true_value.As<BooleanObject>();
  CHECK(!true_boolean_object->IsBoolean());
  CHECK(true_boolean_object->IsBooleanObject());
  CHECK(true_boolean_object->BooleanValue(isolate));
  CHECK(true_boolean_object->ValueOf());
  CHECK(!true_boolean_object->IsTrue());
  CHECK(!true_boolean_object->IsFalse());
}


THREADED_TEST(Number) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  double PI = 3.1415926;
  Local<v8::Number> pi_obj = v8::Number::New(env->GetIsolate(), PI);
  CHECK_EQ(PI, pi_obj->NumberValue(env.local()).FromJust());
}


THREADED_TEST(ToNumber) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<String> str = v8_str("3.1415926");
  CHECK_EQ(3.1415926, str->NumberValue(env.local()).FromJust());
  v8::Local<v8::Boolean> t = v8::True(isolate);
  CHECK_EQ(1.0, t->NumberValue(env.local()).FromJust());
  v8::Local<v8::Boolean> f = v8::False(isolate);
  CHECK_EQ(0.0, f->NumberValue(env.local()).FromJust());
}


THREADED_TEST(Date) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  double PI = 3.1415926;
  Local<Value> date = v8::Date::New(env.local(), PI).ToLocalChecked();
  CHECK_EQ(3.0, date->NumberValue(env.local()).FromJust());
  CHECK(date.As<v8::Date>()
            ->Set(env.local(), v8_str("property"),
                  v8::Integer::New(env->GetIsolate(), 42))
            .FromJust());
  CHECK_EQ(42, date.As<v8::Date>()
                   ->Get(env.local(), v8_str("property"))
                   .ToLocalChecked()
                   ->Int32Value(env.local())
                   .FromJust());
}


THREADED_TEST(Boolean) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Boolean> t = v8::True(isolate);
  CHECK(t->Value());
  v8::Local<v8::Boolean> f = v8::False(isolate);
  CHECK(!f->Value());
  v8::Local<v8::Primitive> u = v8::Undefined(isolate);
  CHECK(!u->BooleanValue(isolate));
  v8::Local<v8::Primitive> n = v8::Null(isolate);
  CHECK(!n->BooleanValue(isolate));
  v8::Local<String> str1 = v8_str("");
  CHECK(!str1->BooleanValue(isolate));
  v8::Local<String> str2 = v8_str("x");
  CHECK(str2->BooleanValue(isolate));
  CHECK(!v8::Number::New(isolate, 0)->BooleanValue(isolate));
  CHECK(v8::Number::New(isolate, -1)->BooleanValue(isolate));
  CHECK(v8::Number::New(isolate, 1)->BooleanValue(isolate));
  CHECK(v8::Number::New(isolate, 42)->BooleanValue(isolate));
  CHECK(!v8_compile("NaN")
             ->Run(env.local())
             .ToLocalChecked()
             ->BooleanValue(isolate));
}


static void DummyCallHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(13.4));
}


static void GetM(Local<String> name,
                 const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(876));
}


THREADED_TEST(GlobalPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> func_templ =
      v8::FunctionTemplate::New(isolate);
  func_templ->PrototypeTemplate()->Set(
      isolate, "dummy", v8::FunctionTemplate::New(isolate, DummyCallHandler));
  v8::Local<ObjectTemplate> templ = func_templ->InstanceTemplate();
  templ->Set(isolate, "x", v8_num(200));
  templ->SetAccessor(v8_str("m"), GetM);
  LocalContext env(nullptr, templ);
  v8::Local<Script> script(v8_compile("dummy()"));
  v8::Local<Value> result(script->Run(env.local()).ToLocalChecked());
  CHECK_EQ(13.4, result->NumberValue(env.local()).FromJust());
  CHECK_EQ(200, v8_run_int32value(v8_compile("x")));
  CHECK_EQ(876, v8_run_int32value(v8_compile("m")));
}


THREADED_TEST(ObjectTemplate) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> acc =
      v8::FunctionTemplate::New(isolate, Returns42);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("acc"),
                  acc->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  Local<v8::FunctionTemplate> fun = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::String> class_name = v8_str("the_class_name");
  fun->SetClassName(class_name);
  Local<ObjectTemplate> templ1 = ObjectTemplate::New(isolate, fun);
  templ1->Set(isolate, "x", v8_num(10));
  templ1->Set(isolate, "y", v8_num(13));
  templ1->Set(v8_str("foo"), acc);
  Local<v8::Object> instance1 =
      templ1->NewInstance(env.local()).ToLocalChecked();
  CHECK(class_name->StrictEquals(instance1->GetConstructorName()));
  CHECK(env->Global()->Set(env.local(), v8_str("p"), instance1).FromJust());
  CHECK(CompileRun("(p.x == 10)")->BooleanValue(isolate));
  CHECK(CompileRun("(p.y == 13)")->BooleanValue(isolate));
  CHECK(CompileRun("(p.foo() == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(p.foo == acc)")->BooleanValue(isolate));
  // Ensure that foo become a data field.
  CompileRun("p.foo = function() {}");
  Local<v8::FunctionTemplate> fun2 = v8::FunctionTemplate::New(isolate);
  fun2->PrototypeTemplate()->Set(isolate, "nirk", v8_num(123));
  Local<ObjectTemplate> templ2 = fun2->InstanceTemplate();
  templ2->Set(isolate, "a", v8_num(12));
  templ2->Set(isolate, "b", templ1);
  templ2->Set(v8_str("bar"), acc);
  templ2->SetAccessorProperty(v8_str("acc"), acc);
  Local<v8::Object> instance2 =
      templ2->NewInstance(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("q"), instance2).FromJust());
  CHECK(CompileRun("(q.nirk == 123)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.a == 12)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.b.x == 10)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.b.y == 13)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.b.foo() == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.b.foo === acc)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.b !== p)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.bar() == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q.bar == acc)")->BooleanValue(isolate));

  instance2 = templ2->NewInstance(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("q2"), instance2).FromJust());
  CHECK(CompileRun("(q2.nirk == 123)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.a == 12)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.b.x == 10)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.b.y == 13)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.b.foo() == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.b.foo === acc)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.bar() == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(q2.bar === acc)")->BooleanValue(isolate));

  CHECK(CompileRun("(q.b !== q2.b)")->BooleanValue(isolate));
  CHECK(CompileRun("q.b.x = 17; (q2.b.x == 10)")->BooleanValue(isolate));
  CHECK(CompileRun("desc1 = Object.getOwnPropertyDescriptor(q, 'acc');"
                   "(desc1.get === acc)")
            ->BooleanValue(isolate));
  CHECK(CompileRun("desc2 = Object.getOwnPropertyDescriptor(q2, 'acc');"
                   "(desc2.get === acc)")
            ->BooleanValue(isolate));
}

THREADED_TEST(IntegerValue) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  CHECK_EQ(0, CompileRun("undefined")->IntegerValue(env.local()).FromJust());
}

static void GetNirk(Local<String> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(900));
}

static void GetRino(Local<String> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(560));
}

enum ObjectInstantiationMode {
  // Create object using ObjectTemplate::NewInstance.
  ObjectTemplate_NewInstance,
  // Create object using FunctionTemplate::NewInstance on constructor.
  Constructor_GetFunction_NewInstance,
  // Create object using new operator on constructor.
  Constructor_GetFunction_New
};

// Test object instance creation using a function template with an instance
// template inherited from another function template with accessors and data
// properties in prototype template.
static void TestObjectTemplateInheritedWithPrototype(
    ObjectInstantiationMode mode) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> fun_A = v8::FunctionTemplate::New(isolate);
  fun_A->SetClassName(v8_str("A"));
  v8::Local<v8::ObjectTemplate> prototype_templ = fun_A->PrototypeTemplate();
  prototype_templ->Set(isolate, "a", v8_num(113));
  prototype_templ->SetNativeDataProperty(v8_str("nirk"), GetNirk);
  prototype_templ->Set(isolate, "b", v8_num(153));

  Local<v8::FunctionTemplate> fun_B = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::String> class_name = v8_str("B");
  fun_B->SetClassName(class_name);
  fun_B->Inherit(fun_A);
  prototype_templ = fun_B->PrototypeTemplate();
  prototype_templ->Set(isolate, "c", v8_num(713));
  prototype_templ->SetNativeDataProperty(v8_str("rino"), GetRino);
  prototype_templ->Set(isolate, "d", v8_num(753));

  Local<ObjectTemplate> templ = fun_B->InstanceTemplate();
  templ->Set(isolate, "x", v8_num(10));
  templ->Set(isolate, "y", v8_num(13));

  // Perform several iterations to trigger creation from cached boilerplate.
  for (int i = 0; i < 3; i++) {
    Local<v8::Object> instance;
    switch (mode) {
      case ObjectTemplate_NewInstance:
        instance = templ->NewInstance(env.local()).ToLocalChecked();
        break;

      case Constructor_GetFunction_NewInstance: {
        Local<v8::Function> function_B =
            fun_B->GetFunction(env.local()).ToLocalChecked();
        instance = function_B->NewInstance(env.local()).ToLocalChecked();
        break;
      }
      case Constructor_GetFunction_New: {
        Local<v8::Function> function_B =
            fun_B->GetFunction(env.local()).ToLocalChecked();
        if (i == 0) {
          CHECK(env->Global()
                    ->Set(env.local(), class_name, function_B)
                    .FromJust());
        }
        instance =
            CompileRun("new B()")->ToObject(env.local()).ToLocalChecked();
        break;
      }
      default:
        UNREACHABLE();
    }

    CHECK(class_name->StrictEquals(instance->GetConstructorName()));
    CHECK(env->Global()->Set(env.local(), v8_str("o"), instance).FromJust());

    CHECK_EQ(10, CompileRun("o.x")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(13, CompileRun("o.y")->IntegerValue(env.local()).FromJust());

    CHECK_EQ(113, CompileRun("o.a")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(900, CompileRun("o.nirk")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(153, CompileRun("o.b")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(713, CompileRun("o.c")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(560, CompileRun("o.rino")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(753, CompileRun("o.d")->IntegerValue(env.local()).FromJust());
  }
}

THREADED_TEST(TestObjectTemplateInheritedWithAccessorsInPrototype1) {
  TestObjectTemplateInheritedWithPrototype(ObjectTemplate_NewInstance);
}

THREADED_TEST(TestObjectTemplateInheritedWithAccessorsInPrototype2) {
  TestObjectTemplateInheritedWithPrototype(Constructor_GetFunction_NewInstance);
}

THREADED_TEST(TestObjectTemplateInheritedWithAccessorsInPrototype3) {
  TestObjectTemplateInheritedWithPrototype(Constructor_GetFunction_New);
}

// Test object instance creation using a function template without an instance
// template inherited from another function template.
static void TestObjectTemplateInheritedWithoutInstanceTemplate(
    ObjectInstantiationMode mode) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> fun_A = v8::FunctionTemplate::New(isolate);
  fun_A->SetClassName(v8_str("A"));

  Local<ObjectTemplate> templ_A = fun_A->InstanceTemplate();
  templ_A->SetNativeDataProperty(v8_str("nirk"), GetNirk);
  templ_A->SetNativeDataProperty(v8_str("rino"), GetRino);

  Local<v8::FunctionTemplate> fun_B = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::String> class_name = v8_str("B");
  fun_B->SetClassName(class_name);
  fun_B->Inherit(fun_A);

  // Perform several iterations to trigger creation from cached boilerplate.
  for (int i = 0; i < 3; i++) {
    Local<v8::Object> instance;
    switch (mode) {
      case Constructor_GetFunction_NewInstance: {
        Local<v8::Function> function_B =
            fun_B->GetFunction(env.local()).ToLocalChecked();
        instance = function_B->NewInstance(env.local()).ToLocalChecked();
        break;
      }
      case Constructor_GetFunction_New: {
        Local<v8::Function> function_B =
            fun_B->GetFunction(env.local()).ToLocalChecked();
        if (i == 0) {
          CHECK(env->Global()
                    ->Set(env.local(), class_name, function_B)
                    .FromJust());
        }
        instance =
            CompileRun("new B()")->ToObject(env.local()).ToLocalChecked();
        break;
      }
      default:
        UNREACHABLE();
    }

    CHECK(class_name->StrictEquals(instance->GetConstructorName()));
    CHECK(env->Global()->Set(env.local(), v8_str("o"), instance).FromJust());

    CHECK_EQ(900, CompileRun("o.nirk")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(560, CompileRun("o.rino")->IntegerValue(env.local()).FromJust());
  }
}

THREADED_TEST(TestObjectTemplateInheritedWithPrototype1) {
  TestObjectTemplateInheritedWithoutInstanceTemplate(
      Constructor_GetFunction_NewInstance);
}

THREADED_TEST(TestObjectTemplateInheritedWithPrototype2) {
  TestObjectTemplateInheritedWithoutInstanceTemplate(
      Constructor_GetFunction_New);
}

THREADED_TEST(TestObjectTemplateClassInheritance) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> fun_A = v8::FunctionTemplate::New(isolate);
  fun_A->SetClassName(v8_str("A"));

  Local<ObjectTemplate> templ_A = fun_A->InstanceTemplate();
  templ_A->SetNativeDataProperty(v8_str("nirk"), GetNirk);
  templ_A->SetNativeDataProperty(v8_str("rino"), GetRino);

  Local<v8::FunctionTemplate> fun_B = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::String> class_name = v8_str("B");
  fun_B->SetClassName(class_name);
  fun_B->Inherit(fun_A);

  v8::Local<v8::String> subclass_name = v8_str("C");
  v8::Local<v8::Object> b_proto;
  v8::Local<v8::Object> c_proto;
  // Perform several iterations to make sure the cache doesn't break
  // subclassing.
  for (int i = 0; i < 3; i++) {
    Local<v8::Function> function_B =
        fun_B->GetFunction(env.local()).ToLocalChecked();
    if (i == 0) {
      CHECK(env->Global()->Set(env.local(), class_name, function_B).FromJust());
      CompileRun("class C extends B {}");
      b_proto =
          CompileRun("B.prototype")->ToObject(env.local()).ToLocalChecked();
      c_proto =
          CompileRun("C.prototype")->ToObject(env.local()).ToLocalChecked();
      CHECK(b_proto->Equals(env.local(), c_proto->GetPrototype()).FromJust());
    }
    Local<v8::Object> instance =
        CompileRun("new C()")->ToObject(env.local()).ToLocalChecked();
    CHECK(c_proto->Equals(env.local(), instance->GetPrototype()).FromJust());

    CHECK(subclass_name->StrictEquals(instance->GetConstructorName()));
    CHECK(env->Global()->Set(env.local(), v8_str("o"), instance).FromJust());

    CHECK_EQ(900, CompileRun("o.nirk")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(560, CompileRun("o.rino")->IntegerValue(env.local()).FromJust());
  }
}

static void NamedPropertyGetterWhichReturns42(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(42));
}

THREADED_TEST(TestObjectTemplateReflectConstruct) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> fun_B = v8::FunctionTemplate::New(isolate);
  fun_B->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(NamedPropertyGetterWhichReturns42));
  v8::Local<v8::String> class_name = v8_str("B");
  fun_B->SetClassName(class_name);

  v8::Local<v8::String> subclass_name = v8_str("C");
  v8::Local<v8::Object> b_proto;
  v8::Local<v8::Object> c_proto;
  // Perform several iterations to make sure the cache doesn't break
  // subclassing.
  for (int i = 0; i < 3; i++) {
    Local<v8::Function> function_B =
        fun_B->GetFunction(env.local()).ToLocalChecked();
    if (i == 0) {
      CHECK(env->Global()->Set(env.local(), class_name, function_B).FromJust());
      CompileRun("function C() {}");
      c_proto =
          CompileRun("C.prototype")->ToObject(env.local()).ToLocalChecked();
    }
    Local<v8::Object> instance = CompileRun("Reflect.construct(B, [], C)")
                                     ->ToObject(env.local())
                                     .ToLocalChecked();
    CHECK(c_proto->Equals(env.local(), instance->GetPrototype()).FromJust());

    CHECK(subclass_name->StrictEquals(instance->GetConstructorName()));
    CHECK(env->Global()->Set(env.local(), v8_str("o"), instance).FromJust());

    CHECK_EQ(42, CompileRun("o.nirk")->IntegerValue(env.local()).FromJust());
    CHECK_EQ(42, CompileRun("o.rino")->IntegerValue(env.local()).FromJust());
  }
}

static void GetFlabby(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(17.2));
}


static void GetKnurd(Local<String> property,
                     const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(15.2));
}


THREADED_TEST(DescriptorInheritance) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> super = v8::FunctionTemplate::New(isolate);
  super->PrototypeTemplate()->Set(isolate, "flabby",
                                  v8::FunctionTemplate::New(isolate,
                                                            GetFlabby));
  super->PrototypeTemplate()->Set(isolate, "PI", v8_num(3.14));

  super->InstanceTemplate()->SetAccessor(v8_str("knurd"), GetKnurd);

  v8::Local<v8::FunctionTemplate> base1 = v8::FunctionTemplate::New(isolate);
  base1->Inherit(super);
  base1->PrototypeTemplate()->Set(isolate, "v1", v8_num(20.1));

  v8::Local<v8::FunctionTemplate> base2 = v8::FunctionTemplate::New(isolate);
  base2->Inherit(super);
  base2->PrototypeTemplate()->Set(isolate, "v2", v8_num(10.1));

  LocalContext env;

  CHECK(env->Global()
            ->Set(env.local(), v8_str("s"),
                  super->GetFunction(env.local()).ToLocalChecked())
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("base1"),
                  base1->GetFunction(env.local()).ToLocalChecked())
            .FromJust());
  CHECK(env->Global()
            ->Set(env.local(), v8_str("base2"),
                  base2->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  // Checks right __proto__ chain.
  CHECK(CompileRun("base1.prototype.__proto__ == s.prototype")
            ->BooleanValue(isolate));
  CHECK(CompileRun("base2.prototype.__proto__ == s.prototype")
            ->BooleanValue(isolate));

  CHECK(v8_compile("s.prototype.PI == 3.14")
            ->Run(env.local())
            .ToLocalChecked()
            ->BooleanValue(isolate));

  // Instance accessor should not be visible on function object or its prototype
  CHECK(CompileRun("s.knurd == undefined")->BooleanValue(isolate));
  CHECK(CompileRun("s.prototype.knurd == undefined")->BooleanValue(isolate));
  CHECK(
      CompileRun("base1.prototype.knurd == undefined")->BooleanValue(isolate));

  CHECK(env->Global()
            ->Set(env.local(), v8_str("obj"), base1->GetFunction(env.local())
                                                  .ToLocalChecked()
                                                  ->NewInstance(env.local())
                                                  .ToLocalChecked())
            .FromJust());
  CHECK_EQ(17.2,
           CompileRun("obj.flabby()")->NumberValue(env.local()).FromJust());
  CHECK(CompileRun("'flabby' in obj")->BooleanValue(isolate));
  CHECK_EQ(15.2, CompileRun("obj.knurd")->NumberValue(env.local()).FromJust());
  CHECK(CompileRun("'knurd' in obj")->BooleanValue(isolate));
  CHECK_EQ(20.1, CompileRun("obj.v1")->NumberValue(env.local()).FromJust());

  CHECK(env->Global()
            ->Set(env.local(), v8_str("obj2"), base2->GetFunction(env.local())
                                                   .ToLocalChecked()
                                                   ->NewInstance(env.local())
                                                   .ToLocalChecked())
            .FromJust());
  CHECK_EQ(17.2,
           CompileRun("obj2.flabby()")->NumberValue(env.local()).FromJust());
  CHECK(CompileRun("'flabby' in obj2")->BooleanValue(isolate));
  CHECK_EQ(15.2, CompileRun("obj2.knurd")->NumberValue(env.local()).FromJust());
  CHECK(CompileRun("'knurd' in obj2")->BooleanValue(isolate));
  CHECK_EQ(10.1, CompileRun("obj2.v2")->NumberValue(env.local()).FromJust());

  // base1 and base2 cannot cross reference to each's prototype
  CHECK(CompileRun("obj.v2")->IsUndefined());
  CHECK(CompileRun("obj2.v1")->IsUndefined());
}

THREADED_TEST(DescriptorInheritance2) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> fun_A = v8::FunctionTemplate::New(isolate);
  fun_A->SetClassName(v8_str("A"));
  fun_A->InstanceTemplate()->SetNativeDataProperty(v8_str("knurd1"), GetKnurd);
  fun_A->InstanceTemplate()->SetNativeDataProperty(v8_str("nirk1"), GetNirk);
  fun_A->InstanceTemplate()->SetNativeDataProperty(v8_str("rino1"), GetRino);

  v8::Local<v8::FunctionTemplate> fun_B = v8::FunctionTemplate::New(isolate);
  fun_B->SetClassName(v8_str("B"));
  fun_B->Inherit(fun_A);

  v8::Local<v8::FunctionTemplate> fun_C = v8::FunctionTemplate::New(isolate);
  fun_C->SetClassName(v8_str("C"));
  fun_C->Inherit(fun_B);
  fun_C->InstanceTemplate()->SetNativeDataProperty(v8_str("knurd2"), GetKnurd);
  fun_C->InstanceTemplate()->SetNativeDataProperty(v8_str("nirk2"), GetNirk);
  fun_C->InstanceTemplate()->SetNativeDataProperty(v8_str("rino2"), GetRino);

  v8::Local<v8::FunctionTemplate> fun_D = v8::FunctionTemplate::New(isolate);
  fun_D->SetClassName(v8_str("D"));
  fun_D->Inherit(fun_C);

  v8::Local<v8::FunctionTemplate> fun_E = v8::FunctionTemplate::New(isolate);
  fun_E->SetClassName(v8_str("E"));
  fun_E->Inherit(fun_D);
  fun_E->InstanceTemplate()->SetNativeDataProperty(v8_str("knurd3"), GetKnurd);
  fun_E->InstanceTemplate()->SetNativeDataProperty(v8_str("nirk3"), GetNirk);
  fun_E->InstanceTemplate()->SetNativeDataProperty(v8_str("rino3"), GetRino);

  v8::Local<v8::FunctionTemplate> fun_F = v8::FunctionTemplate::New(isolate);
  fun_F->SetClassName(v8_str("F"));
  fun_F->Inherit(fun_E);
  v8::Local<v8::ObjectTemplate> templ = fun_F->InstanceTemplate();
  const int kDataPropertiesNumber = 100;
  for (int i = 0; i < kDataPropertiesNumber; i++) {
    v8::Local<v8::Value> val = v8_num(i);
    v8::Local<v8::String> val_str = val->ToString(env.local()).ToLocalChecked();
    v8::Local<v8::String> name = String::Concat(isolate, v8_str("p"), val_str);

    templ->Set(name, val);
    templ->Set(val_str, val);
  }

  CHECK(env->Global()
            ->Set(env.local(), v8_str("F"),
                  fun_F->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  v8::Local<v8::Script> script = v8_compile("o = new F()");

  for (int i = 0; i < 100; i++) {
    v8::HandleScope scope(isolate);
    script->Run(env.local()).ToLocalChecked();
  }
  v8::Local<v8::Object> object = script->Run(env.local())
                                     .ToLocalChecked()
                                     ->ToObject(env.local())
                                     .ToLocalChecked();

  CHECK_EQ(15.2, CompileRun("o.knurd1")->NumberValue(env.local()).FromJust());
  CHECK_EQ(15.2, CompileRun("o.knurd2")->NumberValue(env.local()).FromJust());
  CHECK_EQ(15.2, CompileRun("o.knurd3")->NumberValue(env.local()).FromJust());

  CHECK_EQ(900, CompileRun("o.nirk1")->IntegerValue(env.local()).FromJust());
  CHECK_EQ(900, CompileRun("o.nirk2")->IntegerValue(env.local()).FromJust());
  CHECK_EQ(900, CompileRun("o.nirk3")->IntegerValue(env.local()).FromJust());

  CHECK_EQ(560, CompileRun("o.rino1")->IntegerValue(env.local()).FromJust());
  CHECK_EQ(560, CompileRun("o.rino2")->IntegerValue(env.local()).FromJust());
  CHECK_EQ(560, CompileRun("o.rino3")->IntegerValue(env.local()).FromJust());

  for (int i = 0; i < kDataPropertiesNumber; i++) {
    v8::Local<v8::Value> val = v8_num(i);
    v8::Local<v8::String> val_str = val->ToString(env.local()).ToLocalChecked();
    v8::Local<v8::String> name = String::Concat(isolate, v8_str("p"), val_str);

    CHECK_EQ(i, object->Get(env.local(), name)
                    .ToLocalChecked()
                    ->IntegerValue(env.local())
                    .FromJust());
    CHECK_EQ(i, object->Get(env.local(), val)
                    .ToLocalChecked()
                    ->IntegerValue(env.local())
                    .FromJust());
  }
}


// Helper functions for Interceptor/Accessor interaction tests

void SimpleAccessorGetter(Local<String> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  Local<Object> self = Local<Object>::Cast(info.This());
  info.GetReturnValue().Set(
      self->Get(info.GetIsolate()->GetCurrentContext(),
                String::Concat(info.GetIsolate(), v8_str("accessor_"), name))
          .ToLocalChecked());
}

void SimpleAccessorSetter(Local<String> name, Local<Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
  Local<Object> self = Local<Object>::Cast(info.This());
  CHECK(self->Set(info.GetIsolate()->GetCurrentContext(),
                  String::Concat(info.GetIsolate(), v8_str("accessor_"), name),
                  value)
            .FromJust());
}

void SymbolAccessorGetter(Local<Name> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined())
    return;
  SimpleAccessorGetter(Local<String>::Cast(sym->Name()), info);
}

void SymbolAccessorSetter(Local<Name> name, Local<Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined())
    return;
  SimpleAccessorSetter(Local<String>::Cast(sym->Name()), value, info);
}

void SymbolAccessorGetterReturnsDefault(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined()) return;
  info.GetReturnValue().Set(info.Data());
}

static void ThrowingSymbolAccessorGetter(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(info.GetIsolate()->ThrowException(name));
}


THREADED_TEST(AccessorIsPreservedOnAttributeChange) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;
  v8::Local<v8::Value> res = CompileRun("var a = []; a;");
  i::Handle<i::JSReceiver> a(v8::Utils::OpenHandle(v8::Object::Cast(*res)));
  CHECK_EQ(1, a->map().instance_descriptors().number_of_descriptors());
  CompileRun("Object.defineProperty(a, 'length', { writable: false });");
  CHECK_EQ(0, a->map().instance_descriptors().number_of_descriptors());
  // But we should still have an AccessorInfo.
  i::Handle<i::String> name(v8::Utils::OpenHandle(*v8_str("length")));
  i::LookupIterator it(CcTest::i_isolate(), a, name,
                       i::LookupIterator::OWN_SKIP_INTERCEPTOR);
  CHECK_EQ(i::LookupIterator::ACCESSOR, it.state());
  CHECK(it.GetAccessors()->IsAccessorInfo());
}


THREADED_TEST(UndefinedIsNotEnumerable) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<Value> result = CompileRun("this.propertyIsEnumerable(undefined)");
  CHECK(result->IsFalse());
}


v8::Local<Script> call_recursively_script;
static const int kTargetRecursionDepth = 100;  // near maximum

static void CallScriptRecursivelyCall(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
  int depth = args.This()
                  ->Get(context, v8_str("depth"))
                  .ToLocalChecked()
                  ->Int32Value(context)
                  .FromJust();
  if (depth == kTargetRecursionDepth) return;
  CHECK(args.This()
            ->Set(context, v8_str("depth"),
                  v8::Integer::New(args.GetIsolate(), depth + 1))
            .FromJust());
  args.GetReturnValue().Set(
      call_recursively_script->Run(context).ToLocalChecked());
}


static void CallFunctionRecursivelyCall(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
  int depth = args.This()
                  ->Get(context, v8_str("depth"))
                  .ToLocalChecked()
                  ->Int32Value(context)
                  .FromJust();
  if (depth == kTargetRecursionDepth) {
    printf("[depth = %d]\n", depth);
    return;
  }
  CHECK(args.This()
            ->Set(context, v8_str("depth"),
                  v8::Integer::New(args.GetIsolate(), depth + 1))
            .FromJust());
  v8::Local<Value> function =
      args.This()
          ->Get(context, v8_str("callFunctionRecursively"))
          .ToLocalChecked();
  args.GetReturnValue().Set(function.As<Function>()
                                ->Call(context, args.This(), 0, nullptr)
                                .ToLocalChecked());
}


THREADED_TEST(DeepCrossLanguageRecursion) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("callScriptRecursively"),
              v8::FunctionTemplate::New(isolate, CallScriptRecursivelyCall));
  global->Set(v8_str("callFunctionRecursively"),
              v8::FunctionTemplate::New(isolate, CallFunctionRecursivelyCall));
  LocalContext env(nullptr, global);

  CHECK(env->Global()
            ->Set(env.local(), v8_str("depth"), v8::Integer::New(isolate, 0))
            .FromJust());
  call_recursively_script = v8_compile("callScriptRecursively()");
  call_recursively_script->Run(env.local()).ToLocalChecked();
  call_recursively_script = v8::Local<Script>();

  CHECK(env->Global()
            ->Set(env.local(), v8_str("depth"), v8::Integer::New(isolate, 0))
            .FromJust());
  CompileRun("callFunctionRecursively()");
}


static void ThrowingPropertyHandlerGet(
    Local<Name> key, const v8::PropertyCallbackInfo<v8::Value>& info) {
  // Since this interceptor is used on "with" objects, the runtime will look up
  // @@unscopables.  Punt.
  if (key->IsSymbol()) return;
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(info.GetIsolate()->ThrowException(key));
}


static void ThrowingPropertyHandlerSet(
    Local<Name> key, Local<Value>,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->ThrowException(key);
  info.GetReturnValue().SetUndefined();  // not the same as empty handle
}


THREADED_TEST(CallbackExceptionRegression) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetHandler(v8::NamedPropertyHandlerConfiguration(
      ThrowingPropertyHandlerGet, ThrowingPropertyHandlerSet));
  LocalContext env;
  CHECK(env->Global()
            ->Set(env.local(), v8_str("obj"),
                  obj->NewInstance(env.local()).ToLocalChecked())
            .FromJust());
  v8::Local<Value> otto =
      CompileRun("try { with (obj) { otto; } } catch (e) { e; }");
  CHECK(v8_str("otto")->Equals(env.local(), otto).FromJust());
  v8::Local<Value> netto =
      CompileRun("try { with (obj) { netto = 4; } } catch (e) { e; }");
  CHECK(v8_str("netto")->Equals(env.local(), netto).FromJust());
}


THREADED_TEST(FunctionPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> Foo = v8::FunctionTemplate::New(isolate);
  Foo->PrototypeTemplate()->Set(v8_str("plak"), v8_num(321));
  LocalContext env;
  CHECK(env->Global()
            ->Set(env.local(), v8_str("Foo"),
                  Foo->GetFunction(env.local()).ToLocalChecked())
            .FromJust());
  Local<Script> script = v8_compile("Foo.prototype.plak");
  CHECK_EQ(v8_run_int32value(script), 321);
}

THREADED_TEST(InternalFields) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetInternalFieldCount(1);
  Local<v8::Object> obj = templ->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK_EQ(1, obj->InternalFieldCount());
  CHECK(obj->GetInternalField(0)->IsUndefined());
  obj->SetInternalField(0, v8_num(17));
  CHECK_EQ(17, obj->GetInternalField(0)->Int32Value(env.local()).FromJust());
}

TEST(InternalFieldsSubclassing) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::HandleScope scope(isolate);
  for (int nof_embedder_fields = 0;
       nof_embedder_fields < i::JSObject::kMaxEmbedderFields;
       nof_embedder_fields++) {
    Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
    Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
    instance_templ->SetInternalFieldCount(nof_embedder_fields);
    Local<Function> constructor =
        templ->GetFunction(env.local()).ToLocalChecked();
    // Check that instances have the correct NOF properties.
    Local<v8::Object> obj =
        constructor->NewInstance(env.local()).ToLocalChecked();

    i::Handle<i::JSObject> i_obj =
        i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*obj));
    CHECK_EQ(nof_embedder_fields, obj->InternalFieldCount());
    CHECK_EQ(0, i_obj->map().GetInObjectProperties());
    // Check writing and reading internal fields.
    for (int j = 0; j < nof_embedder_fields; j++) {
      CHECK(obj->GetInternalField(j)->IsUndefined());
      int value = 17 + j;
      obj->SetInternalField(j, v8_num(value));
    }
    for (int j = 0; j < nof_embedder_fields; j++) {
      int value = 17 + j;
      CHECK_EQ(value,
               obj->GetInternalField(j)->Int32Value(env.local()).FromJust());
    }
    CHECK(env->Global()
              ->Set(env.local(), v8_str("BaseClass"), constructor)
              .FromJust());
    // Create various levels of subclasses to stress instance size calculation.
    const int kMaxNofProperties =
        i::JSObject::kMaxInObjectProperties -
        nof_embedder_fields * i::kEmbedderDataSlotSizeInTaggedSlots;
    // Select only a few values to speed up the test.
    int sizes[] = {0,
                   1,
                   2,
                   3,
                   4,
                   5,
                   6,
                   kMaxNofProperties / 4,
                   kMaxNofProperties / 2,
                   kMaxNofProperties - 2,
                   kMaxNofProperties - 1,
                   kMaxNofProperties + 1,
                   kMaxNofProperties + 2,
                   kMaxNofProperties * 2,
                   kMaxNofProperties * 2};
    for (size_t i = 0; i < arraysize(sizes); i++) {
      int nof_properties = sizes[i];
      bool in_object_only = nof_properties <= kMaxNofProperties;
      std::ostringstream src;
      // Assembler source string for a subclass with {nof_properties}
      // in-object properties.
      src << "(function() {\n"
          << "  class SubClass extends BaseClass {\n"
          << "    constructor() {\n"
          << "      super();\n";
      // Set {nof_properties} instance properties in the constructor.
      for (int j = 0; j < nof_properties; j++) {
        src << "      this.property" << j << " = " << j << ";\n";
      }
      src << "    }\n"
          << "  };\n"
          << "  let instance;\n"
          << "  for (let i = 0; i < 3; i++) {\n"
          << "    instance = new SubClass();\n"
          << "  }"
          << "  return instance;\n"
          << "})();";
      Local<v8::Object> value = CompileRun(src.str().c_str()).As<v8::Object>();

      i::Handle<i::JSObject> i_value =
          i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*value));
#ifdef VERIFY_HEAP
      i_value->HeapObjectVerify(i_isolate);
      i_value->map().HeapObjectVerify(i_isolate);
      i_value->map().FindRootMap(i_isolate).HeapObjectVerify(i_isolate);
#endif
      CHECK_EQ(nof_embedder_fields, value->InternalFieldCount());
      if (in_object_only) {
        CHECK_LE(nof_properties, i_value->map().GetInObjectProperties());
      } else {
        CHECK_LE(i_value->map().GetInObjectProperties(), kMaxNofProperties);
      }

      // Make Sure we get the precise property count.
      i_value->map().FindRootMap(i_isolate).CompleteInobjectSlackTracking(
          i_isolate);
      // TODO(cbruni): fix accounting to make this condition true.
      // CHECK_EQ(0, i_value->map()->UnusedPropertyFields());
      if (in_object_only) {
        CHECK_EQ(nof_properties, i_value->map().GetInObjectProperties());
      } else {
        CHECK_LE(i_value->map().GetInObjectProperties(), kMaxNofProperties);
      }
    }
  }
}

THREADED_TEST(InternalFieldsOfRegularObjects) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  const char* sources[] = {"new Object()", "{ a: 'a property' }", "arguments"};
  for (size_t i = 0; i < arraysize(sources); ++i) {
    i::ScopedVector<char> source(128);
    i::SNPrintF(source, "(function() { return %s })()", sources[i]);
    v8::Local<v8::Object> obj = CompileRun(source.begin()).As<v8::Object>();
    CHECK_EQ(0, obj->InternalFieldCount());
  }
}

THREADED_TEST(GlobalObjectInternalFields) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  global_template->SetInternalFieldCount(1);
  LocalContext env(nullptr, global_template);
  v8::Local<v8::Object> global_proxy = env->Global();
  v8::Local<v8::Object> global = global_proxy->GetPrototype().As<v8::Object>();
  CHECK_EQ(1, global->InternalFieldCount());
  CHECK(global->GetInternalField(0)->IsUndefined());
  global->SetInternalField(0, v8_num(17));
  CHECK_EQ(17, global->GetInternalField(0)->Int32Value(env.local()).FromJust());
}


THREADED_TEST(GlobalObjectHasRealIndexedProperty) {
  LocalContext env;
  v8::HandleScope scope(CcTest::isolate());

  v8::Local<v8::Object> global = env->Global();
  CHECK(global->Set(env.local(), 0, v8_str("value")).FromJust());
  CHECK(global->HasRealIndexedProperty(env.local(), 0).FromJust());
}

static void CheckAlignedPointerInInternalField(Local<v8::Object> obj,
                                               void* value) {
  CHECK(HAS_SMI_TAG(reinterpret_cast<i::Address>(value)));
  obj->SetAlignedPointerInInternalField(0, value);
  CcTest::CollectAllGarbage();
  CHECK_EQ(value, obj->GetAlignedPointerFromInternalField(0));
}

THREADED_TEST(InternalFieldsAlignedPointers) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetInternalFieldCount(1);
  Local<v8::Object> obj = templ->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK_EQ(1, obj->InternalFieldCount());

  CheckAlignedPointerInInternalField(obj, nullptr);

  int* heap_allocated = new int[100];
  CheckAlignedPointerInInternalField(obj, heap_allocated);
  delete[] heap_allocated;

  int stack_allocated[100];
  CheckAlignedPointerInInternalField(obj, stack_allocated);

  void* huge = reinterpret_cast<void*>(~static_cast<uintptr_t>(1));
  CheckAlignedPointerInInternalField(obj, huge);

  v8::Global<v8::Object> persistent(isolate, obj);
  CHECK_EQ(1, Object::InternalFieldCount(persistent));
  CHECK_EQ(huge, Object::GetAlignedPointerFromInternalField(persistent, 0));
}

THREADED_TEST(SetAlignedPointerInInternalFields) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetInternalFieldCount(2);
  Local<v8::Object> obj = templ->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK_EQ(2, obj->InternalFieldCount());

  int* heap_allocated_1 = new int[100];
  int* heap_allocated_2 = new int[100];
  int indices[] = {0, 1};
  void* values[] = {heap_allocated_1, heap_allocated_2};

  obj->SetAlignedPointerInInternalFields(2, indices, values);
  CcTest::CollectAllGarbage();
  {
    v8::SealHandleScope no_handle_leak(isolate);
    CHECK_EQ(heap_allocated_1, obj->GetAlignedPointerFromInternalField(0));
    CHECK_EQ(heap_allocated_2, obj->GetAlignedPointerFromInternalField(1));
  }

  indices[0] = 1;
  indices[1] = 0;
  obj->SetAlignedPointerInInternalFields(2, indices, values);
  CcTest::CollectAllGarbage();
  CHECK_EQ(heap_allocated_2, obj->GetAlignedPointerFromInternalField(0));
  CHECK_EQ(heap_allocated_1, obj->GetAlignedPointerFromInternalField(1));

  delete[] heap_allocated_1;
  delete[] heap_allocated_2;
}

static void CheckAlignedPointerInEmbedderData(LocalContext* env, int index,
                                              void* value) {
  CHECK_EQ(0, static_cast<int>(reinterpret_cast<uintptr_t>(value) & 0x1));
  (*env)->SetAlignedPointerInEmbedderData(index, value);
  CcTest::CollectAllGarbage();
  CHECK_EQ(value, (*env)->GetAlignedPointerFromEmbedderData(index));
}


static void* AlignedTestPointer(int i) {
  return reinterpret_cast<void*>(i * 1234);
}


THREADED_TEST(EmbedderDataAlignedPointers) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CheckAlignedPointerInEmbedderData(&env, 0, nullptr);
  CHECK_EQ(1, (*env)->GetNumberOfEmbedderDataFields());

  int* heap_allocated = new int[100];
  CheckAlignedPointerInEmbedderData(&env, 1, heap_allocated);
  CHECK_EQ(2, (*env)->GetNumberOfEmbedderDataFields());
  delete[] heap_allocated;

  int stack_allocated[100];
  CheckAlignedPointerInEmbedderData(&env, 2, stack_allocated);
  CHECK_EQ(3, (*env)->GetNumberOfEmbedderDataFields());

  void* huge = reinterpret_cast<void*>(~static_cast<uintptr_t>(1));
  CheckAlignedPointerInEmbedderData(&env, 3, huge);
  CHECK_EQ(4, (*env)->GetNumberOfEmbedderDataFields());

  // Test growing of the embedder data's backing store.
  for (int i = 0; i < 100; i++) {
    env->SetAlignedPointerInEmbedderData(i, AlignedTestPointer(i));
  }
  CcTest::CollectAllGarbage();
  for (int i = 0; i < 100; i++) {
    v8::SealHandleScope no_handle_leak(env->GetIsolate());
    CHECK_EQ(AlignedTestPointer(i), env->GetAlignedPointerFromEmbedderData(i));
  }
}

static void CheckEmbedderData(LocalContext* env, int index,
                              v8::Local<Value> data) {
  (*env)->SetEmbedderData(index, data);
  CHECK((*env)->GetEmbedderData(index)->StrictEquals(data));
}


THREADED_TEST(EmbedderData) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  CHECK_EQ(0, (*env)->GetNumberOfEmbedderDataFields());
  CheckEmbedderData(&env, 3, v8_str("The quick brown fox jumps"));
  CHECK_EQ(4, (*env)->GetNumberOfEmbedderDataFields());
  CheckEmbedderData(&env, 2, v8_str("over the lazy dog."));
  CHECK_EQ(4, (*env)->GetNumberOfEmbedderDataFields());
  CheckEmbedderData(&env, 1, v8::Number::New(isolate, 1.2345));
  CHECK_EQ(4, (*env)->GetNumberOfEmbedderDataFields());
  CheckEmbedderData(&env, 0, v8::Boolean::New(isolate, true));
  CHECK_EQ(4, (*env)->GetNumberOfEmbedderDataFields());
  CheckEmbedderData(&env, 211, v8::Boolean::New(isolate, true));
  CHECK_EQ(212, (*env)->GetNumberOfEmbedderDataFields());
}


THREADED_TEST(IdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // Ensure that the test starts with an fresh heap to test whether the hash
  // code is based on the address.
  CcTest::CollectAllGarbage();
  Local<v8::Object> obj = v8::Object::New(isolate);
  int hash = obj->GetIdentityHash();
  int hash1 = obj->GetIdentityHash();
  CHECK_EQ(hash, hash1);
  int hash2 = v8::Object::New(isolate)->GetIdentityHash();
  // Since the identity hash is essentially a random number two consecutive
  // objects should not be assigned the same hash code. If the test below fails
  // the random number generator should be evaluated.
  CHECK_NE(hash, hash2);
  CcTest::CollectAllGarbage();
  int hash3 = v8::Object::New(isolate)->GetIdentityHash();
  // Make sure that the identity hash is not based on the initial address of
  // the object alone. If the test below fails the random number generator
  // should be evaluated.
  CHECK_NE(hash, hash3);
  int hash4 = obj->GetIdentityHash();
  CHECK_EQ(hash, hash4);

  // Check identity hashes behaviour in the presence of JS accessors.
  // Put a getter for 'v8::IdentityHash' on the Object's prototype:
  {
    CompileRun("Object.prototype['v8::IdentityHash'] = 42;\n");
    Local<v8::Object> o1 = v8::Object::New(isolate);
    Local<v8::Object> o2 = v8::Object::New(isolate);
    CHECK_NE(o1->GetIdentityHash(), o2->GetIdentityHash());
  }
  {
    CompileRun(
        "function cnst() { return 42; };\n"
        "Object.prototype.__defineGetter__('v8::IdentityHash', cnst);\n");
    Local<v8::Object> o1 = v8::Object::New(isolate);
    Local<v8::Object> o2 = v8::Object::New(isolate);
    CHECK_NE(o1->GetIdentityHash(), o2->GetIdentityHash());
  }
}


void GlobalProxyIdentityHash(bool set_in_js) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::HandleScope scope(isolate);
  Local<Object> global_proxy = env->Global();
  i::Handle<i::Object> i_global_proxy = v8::Utils::OpenHandle(*global_proxy);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("global"), global_proxy)
            .FromJust());
  int32_t hash1;
  if (set_in_js) {
    CompileRun("var m = new Set(); m.add(global);");
    i::Object original_hash = i_global_proxy->GetHash();
    CHECK(original_hash.IsSmi());
    hash1 = i::Smi::ToInt(original_hash);
  } else {
    hash1 = i_global_proxy->GetOrCreateHash(i_isolate).value();
  }
  // Hash should be retained after being detached.
  env->DetachGlobal();
  int hash2 = global_proxy->GetIdentityHash();
  CHECK_EQ(hash1, hash2);
  {
    // Re-attach global proxy to a new context, hash should stay the same.
    LocalContext env2(nullptr, Local<ObjectTemplate>(), global_proxy);
    int hash3 = global_proxy->GetIdentityHash();
    CHECK_EQ(hash1, hash3);
  }
}


THREADED_TEST(GlobalProxyIdentityHash) {
  GlobalProxyIdentityHash(true);
  GlobalProxyIdentityHash(false);
}


TEST(SymbolIdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<v8::Symbol> symbol = v8::Symbol::New(isolate);
    int hash = symbol->GetIdentityHash();
    int hash1 = symbol->GetIdentityHash();
    CHECK_EQ(hash, hash1);
    CcTest::CollectAllGarbage();
    int hash3 = symbol->GetIdentityHash();
    CHECK_EQ(hash, hash3);
  }

  {
    v8::Local<v8::Symbol> js_symbol =
        CompileRun("Symbol('foo')").As<v8::Symbol>();
    int hash = js_symbol->GetIdentityHash();
    int hash1 = js_symbol->GetIdentityHash();
    CHECK_EQ(hash, hash1);
    CcTest::CollectAllGarbage();
    int hash3 = js_symbol->GetIdentityHash();
    CHECK_EQ(hash, hash3);
  }
}


TEST(StringIdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::String> str = v8_str("str1");
  int hash = str->GetIdentityHash();
  int hash1 = str->GetIdentityHash();
  CHECK_EQ(hash, hash1);
  CcTest::CollectAllGarbage();
  int hash3 = str->GetIdentityHash();
  CHECK_EQ(hash, hash3);

  Local<v8::String> str2 = v8_str("str1");
  int hash4 = str2->GetIdentityHash();
  CHECK_EQ(hash, hash4);
}


THREADED_TEST(SymbolProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Local<v8::Symbol> sym1 = v8::Symbol::New(isolate);
  v8::Local<v8::Symbol> sym2 = v8::Symbol::New(isolate, v8_str("my-symbol"));
  v8::Local<v8::Symbol> sym3 = v8::Symbol::New(isolate, v8_str("sym3"));
  v8::Local<v8::Symbol> sym4 = v8::Symbol::New(isolate, v8_str("native"));

  CcTest::CollectAllGarbage();

  // Check basic symbol functionality.
  CHECK(sym1->IsSymbol());
  CHECK(sym2->IsSymbol());
  CHECK(!obj->IsSymbol());

  CHECK(sym1->Equals(env.local(), sym1).FromJust());
  CHECK(sym2->Equals(env.local(), sym2).FromJust());
  CHECK(!sym1->Equals(env.local(), sym2).FromJust());
  CHECK(!sym2->Equals(env.local(), sym1).FromJust());
  CHECK(sym1->StrictEquals(sym1));
  CHECK(sym2->StrictEquals(sym2));
  CHECK(!sym1->StrictEquals(sym2));
  CHECK(!sym2->StrictEquals(sym1));

  CHECK(sym2->Name()->Equals(env.local(), v8_str("my-symbol")).FromJust());

  v8::Local<v8::Value> sym_val = sym2;
  CHECK(sym_val->IsSymbol());
  CHECK(sym_val->Equals(env.local(), sym2).FromJust());
  CHECK(sym_val->StrictEquals(sym2));
  CHECK(v8::Symbol::Cast(*sym_val)->Equals(env.local(), sym2).FromJust());

  v8::Local<v8::Value> sym_obj = v8::SymbolObject::New(isolate, sym2);
  CHECK(sym_obj->IsSymbolObject());
  CHECK(!sym2->IsSymbolObject());
  CHECK(!obj->IsSymbolObject());
  CHECK(sym_obj->Equals(env.local(), sym2).FromJust());
  CHECK(!sym_obj->StrictEquals(sym2));
  CHECK(v8::SymbolObject::Cast(*sym_obj)
            ->Equals(env.local(), sym_obj)
            .FromJust());
  CHECK(v8::SymbolObject::Cast(*sym_obj)
            ->ValueOf()
            ->Equals(env.local(), sym2)
            .FromJust());

  // Make sure delete of a non-existent symbol property works.
  CHECK(obj->Delete(env.local(), sym1).FromJust());
  CHECK(!obj->Has(env.local(), sym1).FromJust());

  CHECK(
      obj->Set(env.local(), sym1, v8::Integer::New(isolate, 1503)).FromJust());
  CHECK(obj->Has(env.local(), sym1).FromJust());
  CHECK_EQ(1503, obj->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(
      obj->Set(env.local(), sym1, v8::Integer::New(isolate, 2002)).FromJust());
  CHECK(obj->Has(env.local(), sym1).FromJust());
  CHECK_EQ(2002, obj->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(v8::None, obj->GetPropertyAttributes(env.local(), sym1).FromJust());

  CHECK_EQ(0u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  unsigned num_props =
      obj->GetPropertyNames(env.local()).ToLocalChecked()->Length();
  CHECK(obj->Set(env.local(), v8_str("bla"), v8::Integer::New(isolate, 20))
            .FromJust());
  CHECK_EQ(1u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  CHECK_EQ(num_props + 1,
           obj->GetPropertyNames(env.local()).ToLocalChecked()->Length());

  CcTest::CollectAllGarbage();

  CHECK(obj->SetAccessor(env.local(), sym3, SymbolAccessorGetter,
                         SymbolAccessorSetter)
            .FromJust());
  CHECK(obj->Get(env.local(), sym3).ToLocalChecked()->IsUndefined());
  CHECK(obj->Set(env.local(), sym3, v8::Integer::New(isolate, 42)).FromJust());
  CHECK(obj->Get(env.local(), sym3)
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());
  CHECK(obj->Get(env.local(), v8_str("accessor_sym3"))
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());

  CHECK(obj->SetNativeDataProperty(env.local(), sym4, SymbolAccessorGetter)
            .FromJust());
  CHECK(obj->Get(env.local(), sym4).ToLocalChecked()->IsUndefined());
  CHECK(obj->Set(env.local(), v8_str("accessor_native"),
                 v8::Integer::New(isolate, 123))
            .FromJust());
  CHECK_EQ(123, obj->Get(env.local(), sym4)
                    .ToLocalChecked()
                    ->Int32Value(env.local())
                    .FromJust());
  CHECK(obj->Set(env.local(), sym4, v8::Integer::New(isolate, 314)).FromJust());
  CHECK(obj->Get(env.local(), sym4)
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 314))
            .FromJust());
  CHECK(obj->Delete(env.local(), v8_str("accessor_native")).FromJust());

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(
      obj->Set(env.local(), sym2, v8::Integer::New(isolate, 2008)).FromJust());
  CHECK_EQ(2002, obj->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2008, obj->Get(env.local(), sym2)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2002, obj->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  CHECK(obj->Has(env.local(), sym1).FromJust());
  CHECK(obj->Has(env.local(), sym2).FromJust());
  CHECK(obj->Has(env.local(), sym3).FromJust());
  CHECK(obj->Has(env.local(), v8_str("accessor_sym3")).FromJust());
  CHECK(obj->Delete(env.local(), sym2).FromJust());
  CHECK(obj->Has(env.local(), sym1).FromJust());
  CHECK(!obj->Has(env.local(), sym2).FromJust());
  CHECK(obj->Has(env.local(), sym3).FromJust());
  CHECK(obj->Has(env.local(), v8_str("accessor_sym3")).FromJust());
  CHECK_EQ(2002, obj->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->Get(env.local(), sym3)
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());
  CHECK(obj->Get(env.local(), v8_str("accessor_sym3"))
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());
  CHECK_EQ(2u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  // Symbol properties are inherited.
  v8::Local<v8::Object> child = v8::Object::New(isolate);
  CHECK(child->SetPrototype(env.local(), obj).FromJust());
  CHECK(child->Has(env.local(), sym1).FromJust());
  CHECK_EQ(2002, child->Get(env.local(), sym1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->Get(env.local(), sym3)
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());
  CHECK(obj->Get(env.local(), v8_str("accessor_sym3"))
            .ToLocalChecked()
            ->Equals(env.local(), v8::Integer::New(isolate, 42))
            .FromJust());
  CHECK_EQ(0u,
           child->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
}


THREADED_TEST(SymbolTemplateProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> foo = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::Name> name = v8::Symbol::New(isolate);
  CHECK(!name.IsEmpty());
  foo->PrototypeTemplate()->Set(name, v8::FunctionTemplate::New(isolate));
  v8::Local<v8::Object> new_instance =
      foo->InstanceTemplate()->NewInstance(env.local()).ToLocalChecked();
  CHECK(!new_instance.IsEmpty());
  CHECK(new_instance->Has(env.local(), name).FromJust());
}


THREADED_TEST(PrivatePropertiesOnProxies) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> target = CompileRun("({})").As<v8::Object>();
  v8::Local<v8::Object> handler = CompileRun("({})").As<v8::Object>();

  v8::Local<v8::Proxy> proxy =
      v8::Proxy::New(env.local(), target, handler).ToLocalChecked();

  v8::Local<v8::Private> priv1 = v8::Private::New(isolate);
  v8::Local<v8::Private> priv2 =
      v8::Private::New(isolate, v8_str("my-private"));

  CcTest::CollectAllGarbage();

  CHECK(priv2->Name()
            ->Equals(env.local(),
                     v8::String::NewFromUtf8(isolate, "my-private",
                                             v8::NewStringType::kNormal)
                         .ToLocalChecked())
            .FromJust());

  // Make sure delete of a non-existent private symbol property works.
  proxy->DeletePrivate(env.local(), priv1).FromJust();
  CHECK(!proxy->HasPrivate(env.local(), priv1).FromJust());

  CHECK(proxy->SetPrivate(env.local(), priv1, v8::Integer::New(isolate, 1503))
            .FromJust());
  CHECK(proxy->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(1503, proxy->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(proxy->SetPrivate(env.local(), priv1, v8::Integer::New(isolate, 2002))
            .FromJust());
  CHECK(proxy->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(2002, proxy->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());

  CHECK_EQ(0u,
           proxy->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  unsigned num_props =
      proxy->GetPropertyNames(env.local()).ToLocalChecked()->Length();
  CHECK(proxy->Set(env.local(), v8::String::NewFromUtf8(
                                    isolate, "bla", v8::NewStringType::kNormal)
                                    .ToLocalChecked(),
                   v8::Integer::New(isolate, 20))
            .FromJust());
  CHECK_EQ(1u,
           proxy->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  CHECK_EQ(num_props + 1,
           proxy->GetPropertyNames(env.local()).ToLocalChecked()->Length());

  CcTest::CollectAllGarbage();

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(proxy->SetPrivate(env.local(), priv2, v8::Integer::New(isolate, 2008))
            .FromJust());
  CHECK_EQ(2002, proxy->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2008, proxy->GetPrivate(env.local(), priv2)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2002, proxy->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(1u,
           proxy->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  CHECK(proxy->HasPrivate(env.local(), priv1).FromJust());
  CHECK(proxy->HasPrivate(env.local(), priv2).FromJust());
  CHECK(proxy->DeletePrivate(env.local(), priv2).FromJust());
  CHECK(proxy->HasPrivate(env.local(), priv1).FromJust());
  CHECK(!proxy->HasPrivate(env.local(), priv2).FromJust());
  CHECK_EQ(2002, proxy->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(1u,
           proxy->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  // Private properties are not inherited (for the time being).
  v8::Local<v8::Object> child = v8::Object::New(isolate);
  CHECK(child->SetPrototype(env.local(), proxy).FromJust());
  CHECK(!child->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(0u,
           child->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
}


THREADED_TEST(PrivateProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Local<v8::Private> priv1 = v8::Private::New(isolate);
  v8::Local<v8::Private> priv2 =
      v8::Private::New(isolate, v8_str("my-private"));

  CcTest::CollectAllGarbage();

  CHECK(priv2->Name()
            ->Equals(env.local(),
                     v8::String::NewFromUtf8(isolate, "my-private",
                                             v8::NewStringType::kNormal)
                         .ToLocalChecked())
            .FromJust());

  // Make sure delete of a non-existent private symbol property works.
  obj->DeletePrivate(env.local(), priv1).FromJust();
  CHECK(!obj->HasPrivate(env.local(), priv1).FromJust());

  CHECK(obj->SetPrivate(env.local(), priv1, v8::Integer::New(isolate, 1503))
            .FromJust());
  CHECK(obj->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(1503, obj->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->SetPrivate(env.local(), priv1, v8::Integer::New(isolate, 2002))
            .FromJust());
  CHECK(obj->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());

  CHECK_EQ(0u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  unsigned num_props =
      obj->GetPropertyNames(env.local()).ToLocalChecked()->Length();
  CHECK(obj->Set(env.local(), v8::String::NewFromUtf8(
                                  isolate, "bla", v8::NewStringType::kNormal)
                                  .ToLocalChecked(),
                 v8::Integer::New(isolate, 20))
            .FromJust());
  CHECK_EQ(1u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
  CHECK_EQ(num_props + 1,
           obj->GetPropertyNames(env.local()).ToLocalChecked()->Length());

  CcTest::CollectAllGarbage();

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(obj->SetPrivate(env.local(), priv2, v8::Integer::New(isolate, 2008))
            .FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2008, obj->GetPrivate(env.local(), priv2)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(1u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  CHECK(obj->HasPrivate(env.local(), priv1).FromJust());
  CHECK(obj->HasPrivate(env.local(), priv2).FromJust());
  CHECK(obj->DeletePrivate(env.local(), priv2).FromJust());
  CHECK(obj->HasPrivate(env.local(), priv1).FromJust());
  CHECK(!obj->HasPrivate(env.local(), priv2).FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), priv1)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(1u,
           obj->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());

  // Private properties are not inherited (for the time being).
  v8::Local<v8::Object> child = v8::Object::New(isolate);
  CHECK(child->SetPrototype(env.local(), obj).FromJust());
  CHECK(!child->HasPrivate(env.local(), priv1).FromJust());
  CHECK_EQ(0u,
           child->GetOwnPropertyNames(env.local()).ToLocalChecked()->Length());
}


THREADED_TEST(GlobalSymbols) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> name = v8_str("my-symbol");
  v8::Local<v8::Symbol> glob = v8::Symbol::For(isolate, name);
  v8::Local<v8::Symbol> glob2 = v8::Symbol::For(isolate, name);
  CHECK(glob2->SameValue(glob));

  v8::Local<v8::Symbol> glob_api = v8::Symbol::ForApi(isolate, name);
  v8::Local<v8::Symbol> glob_api2 = v8::Symbol::ForApi(isolate, name);
  CHECK(glob_api2->SameValue(glob_api));
  CHECK(!glob_api->SameValue(glob));

  v8::Local<v8::Symbol> sym = v8::Symbol::New(isolate, name);
  CHECK(!sym->SameValue(glob));

  CompileRun("var sym2 = Symbol.for('my-symbol')");
  v8::Local<Value> sym2 =
      env->Global()->Get(env.local(), v8_str("sym2")).ToLocalChecked();
  CHECK(sym2->SameValue(glob));
  CHECK(!sym2->SameValue(glob_api));
}

THREADED_TEST(GlobalSymbolsNoContext) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> name = v8_str("my-symbol");
  v8::Local<v8::Symbol> glob = v8::Symbol::For(isolate, name);
  v8::Local<v8::Symbol> glob2 = v8::Symbol::For(isolate, name);
  CHECK(glob2->SameValue(glob));

  v8::Local<v8::Symbol> glob_api = v8::Symbol::ForApi(isolate, name);
  v8::Local<v8::Symbol> glob_api2 = v8::Symbol::ForApi(isolate, name);
  CHECK(glob_api2->SameValue(glob_api));
  CHECK(!glob_api->SameValue(glob));
}

static void CheckWellKnownSymbol(v8::Local<v8::Symbol>(*getter)(v8::Isolate*),
                                 const char* name) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Symbol> symbol = getter(isolate);
  std::string script = std::string("var sym = ") + name;
  CompileRun(script.c_str());
  v8::Local<Value> value =
      env->Global()->Get(env.local(), v8_str("sym")).ToLocalChecked();

  CHECK(!value.IsEmpty());
  CHECK(!symbol.IsEmpty());
  CHECK(value->SameValue(symbol));
}


THREADED_TEST(WellKnownSymbols) {
  CheckWellKnownSymbol(v8::Symbol::GetIterator, "Symbol.iterator");
  CheckWellKnownSymbol(v8::Symbol::GetUnscopables, "Symbol.unscopables");
  CheckWellKnownSymbol(v8::Symbol::GetHasInstance, "Symbol.hasInstance");
  CheckWellKnownSymbol(v8::Symbol::GetIsConcatSpreadable,
                       "Symbol.isConcatSpreadable");
  CheckWellKnownSymbol(v8::Symbol::GetMatch, "Symbol.match");
  CheckWellKnownSymbol(v8::Symbol::GetReplace, "Symbol.replace");
  CheckWellKnownSymbol(v8::Symbol::GetSearch, "Symbol.search");
  CheckWellKnownSymbol(v8::Symbol::GetSplit, "Symbol.split");
  CheckWellKnownSymbol(v8::Symbol::GetToPrimitive, "Symbol.toPrimitive");
  CheckWellKnownSymbol(v8::Symbol::GetToStringTag, "Symbol.toStringTag");
}


THREADED_TEST(GlobalPrivates) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> name = v8_str("my-private");
  v8::Local<v8::Private> glob = v8::Private::ForApi(isolate, name);
  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  CHECK(obj->SetPrivate(env.local(), glob, v8::Integer::New(isolate, 3))
            .FromJust());

  v8::Local<v8::Private> glob2 = v8::Private::ForApi(isolate, name);
  CHECK(obj->HasPrivate(env.local(), glob2).FromJust());

  v8::Local<v8::Private> priv = v8::Private::New(isolate, name);
  CHECK(!obj->HasPrivate(env.local(), priv).FromJust());

  CompileRun("var intern = %CreatePrivateSymbol('my-private')");
  v8::Local<Value> intern =
      env->Global()->Get(env.local(), v8_str("intern")).ToLocalChecked();
  CHECK(!obj->Has(env.local(), intern).FromJust());
}

THREADED_TEST(HiddenProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  v8::Local<v8::Private> key =
      v8::Private::ForApi(isolate, v8_str("api-test::hidden-key"));
  v8::Local<v8::String> empty = v8_str("");
  v8::Local<v8::String> prop_name = v8_str("prop_name");

  CcTest::CollectAllGarbage();

  // Make sure delete of a non-existent hidden value works
  obj->DeletePrivate(env.local(), key).FromJust();

  CHECK(obj->SetPrivate(env.local(), key, v8::Integer::New(isolate, 1503))
            .FromJust());
  CHECK_EQ(1503, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->SetPrivate(env.local(), key, v8::Integer::New(isolate, 2002))
            .FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());

  CcTest::CollectAllGarbage();

  // Make sure we do not find the hidden property.
  CHECK(!obj->Has(env.local(), empty).FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->Get(env.local(), empty).ToLocalChecked()->IsUndefined());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(
      obj->Set(env.local(), empty, v8::Integer::New(isolate, 2003)).FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2003, obj->Get(env.local(), empty)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());

  CcTest::CollectAllGarbage();

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(obj->Set(env.local(), prop_name, v8::Integer::New(isolate, 2008))
            .FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2008, obj->Get(env.local(), prop_name)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  CHECK(obj->Delete(env.local(), prop_name).FromJust());
  CHECK_EQ(2002, obj->GetPrivate(env.local(), key)
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());

  CcTest::CollectAllGarbage();

  CHECK(obj->SetPrivate(env.local(), key, v8::Integer::New(isolate, 2002))
            .FromJust());
  CHECK(obj->DeletePrivate(env.local(), key).FromJust());
  CHECK(!obj->HasPrivate(env.local(), key).FromJust());
}


THREADED_TEST(Regress97784) {
  // Regression test for crbug.com/97784
  // Messing with the Object.prototype should not have effect on
  // hidden properties.
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  v8::Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  v8::Local<v8::Private> key =
      v8::Private::New(env->GetIsolate(), v8_str("hidden"));

  CompileRun(
      "set_called = false;"
      "Object.defineProperty("
      "    Object.prototype,"
      "    'hidden',"
      "    {get: function() { return 45; },"
      "     set: function() { set_called = true; }})");

  CHECK(!obj->HasPrivate(env.local(), key).FromJust());
  // Make sure that the getter and setter from Object.prototype is not invoked.
  // If it did we would have full access to the hidden properties in
  // the accessor.
  CHECK(
      obj->SetPrivate(env.local(), key, v8::Integer::New(env->GetIsolate(), 42))
          .FromJust());
  ExpectFalse("set_called");
  CHECK_EQ(42, obj->GetPrivate(env.local(), key)
                   .ToLocalChecked()
                   ->Int32Value(env.local())
                   .FromJust());
}


THREADED_TEST(External) {
  v8::HandleScope scope(CcTest::isolate());
  int x = 3;
  Local<v8::External> ext = v8::External::New(CcTest::isolate(), &x);
  LocalContext env;
  CHECK(env->Global()->Set(env.local(), v8_str("ext"), ext).FromJust());
  Local<Value> reext_obj = CompileRun("this.ext");
  v8::Local<v8::External> reext = reext_obj.As<v8::External>();
  int* ptr = static_cast<int*>(reext->Value());
  CHECK_EQ(3, x);
  *ptr = 10;
  CHECK_EQ(x, 10);

  {
    i::Handle<i::Object> obj = v8::Utils::OpenHandle(*ext);
    CHECK_EQ(i::HeapObject::cast(*obj).map(), CcTest::heap()->external_map());
    CHECK(ext->IsExternal());
    CHECK(!CompileRun("new Set().add(this.ext)").IsEmpty());
    CHECK_EQ(i::HeapObject::cast(*obj).map(), CcTest::heap()->external_map());
    CHECK(ext->IsExternal());
  }

  // Make sure unaligned pointers are wrapped properly.
  char* data = i::StrDup("0123456789");
  Local<v8::Value> zero = v8::External::New(CcTest::isolate(), &data[0]);
  Local<v8::Value> one = v8::External::New(CcTest::isolate(), &data[1]);
  Local<v8::Value> two = v8::External::New(CcTest::isolate(), &data[2]);
  Local<v8::Value> three = v8::External::New(CcTest::isolate(), &data[3]);

  char* char_ptr = reinterpret_cast<char*>(v8::External::Cast(*zero)->Value());
  CHECK_EQ('0', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*one)->Value());
  CHECK_EQ('1', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*two)->Value());
  CHECK_EQ('2', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*three)->Value());
  CHECK_EQ('3', *char_ptr);
  i::DeleteArray(data);
}


THREADED_TEST(GlobalHandle) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(3, v8::Local<String>::New(isolate, global)->Length());
  }
  global.Reset();
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(3, v8::Local<String>::New(isolate, global)->Length());
  }
  global.Reset();
}


THREADED_TEST(ResettingGlobalHandle) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  size_t initial_handle_count = global_handles->handles_count();
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(3, v8::Local<String>::New(isolate, global)->Length());
  }
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("longer"));
  }
  CHECK_EQ(global_handles->handles_count(), initial_handle_count);
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(6, v8::Local<String>::New(isolate, global)->Length());
  }
  global.Reset();
  CHECK_EQ(global_handles->handles_count(), initial_handle_count - 1);
}


THREADED_TEST(ResettingGlobalHandleToEmpty) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  size_t initial_handle_count = global_handles->handles_count();
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(3, v8::Local<String>::New(isolate, global)->Length());
  }
  {
    v8::HandleScope scope(isolate);
    Local<String> empty;
    global.Reset(isolate, empty);
  }
  CHECK(global.IsEmpty());
  CHECK_EQ(global_handles->handles_count(), initial_handle_count - 1);
}


template <class T>
static v8::Global<T> PassUnique(v8::Global<T> unique) {
  return unique.Pass();
}


template <class T>
static v8::Global<T> ReturnUnique(v8::Isolate* isolate,
                                  const v8::Persistent<T>& global) {
  v8::Global<String> unique(isolate, global);
  return unique.Pass();
}


THREADED_TEST(Global) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  size_t initial_handle_count = global_handles->handles_count();
  {
    v8::Global<String> unique(isolate, global);
    CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
    // Test assignment via Pass
    {
      v8::Global<String> copy = unique.Pass();
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
      unique = copy.Pass();
    }
    // Test ctor via Pass
    {
      v8::Global<String> copy(unique.Pass());
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
      unique = copy.Pass();
    }
    // Test pass through function call
    {
      v8::Global<String> copy = PassUnique(unique.Pass());
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
      unique = copy.Pass();
    }
    CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
  }
  // Test pass from function call
  {
    v8::Global<String> unique = ReturnUnique(isolate, global);
    CHECK(unique == global);
    CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
  }
  CHECK_EQ(initial_handle_count, global_handles->handles_count());
  global.Reset();
}


namespace {

class TwoPassCallbackData;
void FirstPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data);
void SecondPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data);

struct GCCallbackMetadata {
  int instance_counter = 0;
  int depth = 0;
  v8::Persistent<v8::Context> context;

  GCCallbackMetadata() {
    auto isolate = CcTest::isolate();
    v8::HandleScope handle_scope(isolate);
    context.Reset(isolate, CcTest::NewContext());
  }

  ~GCCallbackMetadata() {
    CHECK_EQ(0, instance_counter);
    CHECK_EQ(0, depth);
  }

  struct DepthCheck {
    explicit DepthCheck(GCCallbackMetadata* counters) : counters(counters) {
      CHECK_EQ(counters->depth, 0);
      counters->depth++;
    }

    ~DepthCheck() {
      counters->depth--;
      CHECK_EQ(counters->depth, 0);
    }

    GCCallbackMetadata* counters;
  };
};

class TwoPassCallbackData {
 public:
  TwoPassCallbackData(v8::Isolate* isolate, GCCallbackMetadata* metadata)
      : first_pass_called_(false),
        second_pass_called_(false),
        trigger_gc_(false),
        metadata_(metadata) {
    HandleScope scope(isolate);
    i::ScopedVector<char> buffer(40);
    i::SNPrintF(buffer, "%p", static_cast<void*>(this));
    auto string = v8::String::NewFromUtf8(isolate, buffer.begin(),
                                          v8::NewStringType::kNormal)
                      .ToLocalChecked();
    cell_.Reset(isolate, string);
    metadata_->instance_counter++;
  }

  ~TwoPassCallbackData() {
    CHECK(first_pass_called_);
    CHECK(second_pass_called_);
    CHECK(cell_.IsEmpty());
    metadata_->instance_counter--;
  }

  void FirstPass() {
    CHECK(!first_pass_called_);
    CHECK(!second_pass_called_);
    CHECK(!cell_.IsEmpty());
    cell_.Reset();
    first_pass_called_ = true;
  }

  void SecondPass(v8::Isolate* isolate) {
    ApiTestFuzzer::Fuzz();

    GCCallbackMetadata::DepthCheck depth_check(metadata_);
    CHECK(first_pass_called_);
    CHECK(!second_pass_called_);
    CHECK(cell_.IsEmpty());
    second_pass_called_ = true;

    GCCallbackMetadata* metadata = metadata_;
    bool trigger_gc = trigger_gc_;
    delete this;

    {
      // Make sure that running JS works inside the second pass callback.
      v8::HandleScope handle_scope(isolate);
      v8::Context::Scope context_scope(metadata->context.Get(isolate));
      v8::Local<v8::Value> value = CompileRun("(function() { return 42 })()");
      CHECK(value->IsInt32());
      CHECK_EQ(value.As<v8::Int32>()->Value(), 42);
    }

    if (!trigger_gc) return;
    auto data_2 = new TwoPassCallbackData(isolate, metadata);
    data_2->SetWeak();
    CcTest::CollectAllGarbage();
  }

  void SetWeak() {
    cell_.SetWeak(this, FirstPassCallback, v8::WeakCallbackType::kParameter);
  }

  void MarkTriggerGc() { trigger_gc_ = true; }

 private:
  bool first_pass_called_;
  bool second_pass_called_;
  bool trigger_gc_;
  v8::Global<v8::String> cell_;
  GCCallbackMetadata* metadata_;
};


void SecondPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data) {
  data.GetParameter()->SecondPass(data.GetIsolate());
}


void FirstPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data) {
  data.GetParameter()->FirstPass();
  data.SetSecondPassCallback(SecondPassCallback);
}

}  // namespace


TEST(TwoPassPhantomCallbacks) {
  auto isolate = CcTest::isolate();
  GCCallbackMetadata metadata;
  const size_t kLength = 20;
  for (size_t i = 0; i < kLength; ++i) {
    auto data = new TwoPassCallbackData(isolate, &metadata);
    data->SetWeak();
  }
  CHECK_EQ(static_cast<int>(kLength), metadata.instance_counter);
  CcTest::CollectAllGarbage();
  EmptyMessageQueues(isolate);
}


TEST(TwoPassPhantomCallbacksNestedGc) {
  auto isolate = CcTest::isolate();
  GCCallbackMetadata metadata;
  const size_t kLength = 20;
  TwoPassCallbackData* array[kLength];
  for (size_t i = 0; i < kLength; ++i) {
    array[i] = new TwoPassCallbackData(isolate, &metadata);
    array[i]->SetWeak();
  }
  array[5]->MarkTriggerGc();
  array[10]->MarkTriggerGc();
  array[15]->MarkTriggerGc();
  CHECK_EQ(static_cast<int>(kLength), metadata.instance_counter);
  CcTest::CollectAllGarbage();
  EmptyMessageQueues(isolate);
}

// The string creation API methods forbid executing JS code while they are
// on the stack. Make sure that when such a string creation triggers GC,
// the second pass callback can still execute JS as per its API contract.
TEST(TwoPassPhantomCallbacksTriggeredByStringAlloc) {
  auto isolate = CcTest::isolate();
  GCCallbackMetadata metadata;
  auto data = new TwoPassCallbackData(isolate, &metadata);
  data->SetWeak();
  CHECK_EQ(metadata.instance_counter, 1);

  i::ScopedVector<uint8_t> source(200000);
  v8::HandleScope handle_scope(isolate);
  // Creating a few large strings suffices to trigger GC.
  while (metadata.instance_counter == 1) {
    USE(v8::String::NewFromOneByte(isolate, source.begin(),
                                   v8::NewStringType::kNormal,
                                   static_cast<int>(source.size())));
  }
  EmptyMessageQueues(isolate);
}

namespace {

void* IntKeyToVoidPointer(int key) { return reinterpret_cast<void*>(key << 1); }


Local<v8::Object> NewObjectForIntKey(
    v8::Isolate* isolate, const v8::Global<v8::ObjectTemplate>& templ,
    int key) {
  auto local = Local<v8::ObjectTemplate>::New(isolate, templ);
  auto obj = local->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, IntKeyToVoidPointer(key));
  return obj;
}


template <typename K, typename V>
class PhantomStdMapTraits : public v8::StdMapTraits<K, V> {
 public:
  using MapType = typename v8::GlobalValueMap<K, V, PhantomStdMapTraits<K, V>>;
  static const v8::PersistentContainerCallbackType kCallbackType =
      v8::kWeakWithInternalFields;
  struct WeakCallbackDataType {
    MapType* map;
    K key;
  };
  static WeakCallbackDataType* WeakCallbackParameter(MapType* map, const K& key,
                                                     Local<V> value) {
    WeakCallbackDataType* data = new WeakCallbackDataType;
    data->map = map;
    data->key = key;
    return data;
  }
  static MapType* MapFromWeakCallbackInfo(
      const v8::WeakCallbackInfo<WeakCallbackDataType>& data) {
    return data.GetParameter()->map;
  }
  static K KeyFromWeakCallbackInfo(
      const v8::WeakCallbackInfo<WeakCallbackDataType>& data) {
    return data.GetParameter()->key;
  }
  static void DisposeCallbackData(WeakCallbackDataType* data) { delete data; }
  static void Dispose(v8::Isolate* isolate, v8::Global<V> value, K key) {
    CHECK_EQ(IntKeyToVoidPointer(key),
             v8::Object::GetAlignedPointerFromInternalField(value, 0));
  }
  static void OnWeakCallback(
      const v8::WeakCallbackInfo<WeakCallbackDataType>&) {}
  static void DisposeWeak(
      const v8::WeakCallbackInfo<WeakCallbackDataType>& info) {
    K key = KeyFromWeakCallbackInfo(info);
    CHECK_EQ(IntKeyToVoidPointer(key), info.GetInternalField(0));
    DisposeCallbackData(info.GetParameter());
  }
};


template <typename Map>
void TestGlobalValueMap() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Global<ObjectTemplate> templ;
  {
    HandleScope scope(isolate);
    auto t = ObjectTemplate::New(isolate);
    t->SetInternalFieldCount(1);
    templ.Reset(isolate, t);
  }
  Map map(isolate);
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  size_t initial_handle_count = global_handles->handles_count();
  CHECK_EQ(0, static_cast<int>(map.Size()));
  {
    HandleScope scope(isolate);
    Local<v8::Object> obj = map.Get(7);
    CHECK(obj.IsEmpty());
    Local<v8::Object> expected = v8::Object::New(isolate);
    map.Set(7, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    obj = map.Get(7);
    CHECK(expected->Equals(env.local(), obj).FromJust());
    {
      typename Map::PersistentValueReference ref = map.GetReference(7);
      CHECK(expected->Equals(env.local(), ref.NewLocal(isolate)).FromJust());
    }
    v8::Global<v8::Object> removed = map.Remove(7);
    CHECK_EQ(0, static_cast<int>(map.Size()));
    CHECK(expected == removed);
    removed = map.Remove(7);
    CHECK(removed.IsEmpty());
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    {
      typename Map::PersistentValueReference ref;
      Local<v8::Object> expected2 = NewObjectForIntKey(isolate, templ, 8);
      removed = map.Set(8, v8::Global<v8::Object>(isolate, expected2), &ref);
      CHECK_EQ(1, static_cast<int>(map.Size()));
      CHECK(expected == removed);
      CHECK(expected2->Equals(env.local(), ref.NewLocal(isolate)).FromJust());
    }
  }
  CHECK_EQ(initial_handle_count + 1, global_handles->handles_count());
  if (map.IsWeak()) {
    CcTest::PreciseCollectAllGarbage();
  } else {
    map.Clear();
  }
  CHECK_EQ(0, static_cast<int>(map.Size()));
  CHECK_EQ(initial_handle_count, global_handles->handles_count());
  {
    HandleScope scope(isolate);
    Local<v8::Object> value = NewObjectForIntKey(isolate, templ, 9);
    map.Set(9, value);
    map.Clear();
  }
  CHECK_EQ(0, static_cast<int>(map.Size()));
  CHECK_EQ(initial_handle_count, global_handles->handles_count());
}

}  // namespace


TEST(GlobalValueMap) {
  // Default case, w/o weak callbacks:
  TestGlobalValueMap<v8::StdGlobalValueMap<int, v8::Object>>();

  // Custom traits with weak callbacks:
  using WeakMap =
      v8::GlobalValueMap<int, v8::Object, PhantomStdMapTraits<int, v8::Object>>;
  TestGlobalValueMap<WeakMap>();
}


TEST(PersistentValueVector) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  size_t handle_count = global_handles->handles_count();
  HandleScope scope(isolate);

  v8::PersistentValueVector<v8::Object> vector(isolate);

  Local<v8::Object> obj1 = v8::Object::New(isolate);
  Local<v8::Object> obj2 = v8::Object::New(isolate);
  v8::Global<v8::Object> obj3(isolate, v8::Object::New(isolate));

  CHECK(vector.IsEmpty());
  CHECK_EQ(0, static_cast<int>(vector.Size()));

  vector.ReserveCapacity(3);
  CHECK(vector.IsEmpty());

  vector.Append(obj1);
  vector.Append(obj2);
  vector.Append(obj1);
  vector.Append(obj3.Pass());
  vector.Append(obj1);

  CHECK(!vector.IsEmpty());
  CHECK_EQ(5, static_cast<int>(vector.Size()));
  CHECK(obj3.IsEmpty());
  CHECK(obj1->Equals(env.local(), vector.Get(0)).FromJust());
  CHECK(obj1->Equals(env.local(), vector.Get(2)).FromJust());
  CHECK(obj1->Equals(env.local(), vector.Get(4)).FromJust());
  CHECK(obj2->Equals(env.local(), vector.Get(1)).FromJust());

  CHECK_EQ(5 + handle_count, global_handles->handles_count());

  vector.Clear();
  CHECK(vector.IsEmpty());
  CHECK_EQ(0, static_cast<int>(vector.Size()));
  CHECK_EQ(handle_count, global_handles->handles_count());
}


THREADED_TEST(GlobalHandleUpcast) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<String> local = v8::Local<String>::New(isolate, v8_str("str"));
  v8::Persistent<String> global_string(isolate, local);
  v8::Persistent<Value>& global_value =
      v8::Persistent<Value>::Cast(global_string);
  CHECK(v8::Local<v8::Value>::New(isolate, global_value)->IsString());
  CHECK(global_string == v8::Persistent<String>::Cast(global_value));
  global_string.Reset();
}


THREADED_TEST(HandleEquality) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global1;
  v8::Persistent<String> global2;
  {
    v8::HandleScope scope(isolate);
    global1.Reset(isolate, v8_str("str"));
    global2.Reset(isolate, v8_str("str2"));
  }
  CHECK(global1 == global1);
  CHECK(!(global1 != global1));
  {
    v8::HandleScope scope(isolate);
    Local<String> local1 = Local<String>::New(isolate, global1);
    Local<String> local2 = Local<String>::New(isolate, global2);

    CHECK(global1 == local1);
    CHECK(!(global1 != local1));
    CHECK(local1 == global1);
    CHECK(!(local1 != global1));

    CHECK(!(global1 == local2));
    CHECK(global1 != local2);
    CHECK(!(local2 == global1));
    CHECK(local2 != global1);

    CHECK(!(local1 == local2));
    CHECK(local1 != local2);

    Local<String> anotherLocal1 = Local<String>::New(isolate, global1);
    CHECK(local1 == anotherLocal1);
    CHECK(!(local1 != anotherLocal1));
  }
  global1.Reset();
  global2.Reset();
}


THREADED_TEST(LocalHandle) {
  v8::HandleScope scope(CcTest::isolate());
  v8::Local<String> local =
      v8::Local<String>::New(CcTest::isolate(), v8_str("str"));
  CHECK_EQ(3, local->Length());
}


class WeakCallCounter {
 public:
  explicit WeakCallCounter(int id) : id_(id), number_of_weak_calls_(0) {}
  int id() { return id_; }
  void increment() { number_of_weak_calls_++; }
  int NumberOfWeakCalls() { return number_of_weak_calls_; }

 private:
  int id_;
  int number_of_weak_calls_;
};


template <typename T>
struct WeakCallCounterAndPersistent {
  explicit WeakCallCounterAndPersistent(WeakCallCounter* counter)
      : counter(counter) {}
  WeakCallCounter* counter;
  v8::Persistent<T> handle;
};


template <typename T>
static void WeakPointerCallback(
    const v8::WeakCallbackInfo<WeakCallCounterAndPersistent<T>>& data) {
  CHECK_EQ(1234, data.GetParameter()->counter->id());
  data.GetParameter()->counter->increment();
  data.GetParameter()->handle.Reset();
}

THREADED_TEST(ScriptException) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<Script> script = v8_compile("throw 'panama!';");
  v8::TryCatch try_catch(env->GetIsolate());
  v8::MaybeLocal<Value> result = script->Run(env.local());
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(env->GetIsolate(), try_catch.Exception());
  CHECK_EQ(0, strcmp(*exception_value, "panama!"));
}


TEST(TryCatchCustomException) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);
  CompileRun(
      "function CustomError() { this.a = 'b'; }"
      "(function f() { throw new CustomError(); })();");
  CHECK(try_catch.HasCaught());
  CHECK(try_catch.Exception()
            ->ToObject(env.local())
            .ToLocalChecked()
            ->Get(env.local(), v8_str("a"))
            .ToLocalChecked()
            ->Equals(env.local(), v8_str("b"))
            .FromJust());
}


bool message_received;


static void check_message_0(v8::Local<v8::Message> message,
                            v8::Local<Value> data) {
  CHECK_EQ(5.76, data->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  CHECK_EQ(6.75, message->GetScriptOrigin()
                     .ResourceName()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


THREADED_TEST(MessageHandler0) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  LocalContext context;
  CcTest::isolate()->AddMessageListener(check_message_0, v8_num(5.76));
  v8::Local<v8::Script> script =
      CompileWithOrigin("throw 'error'", "6.75", false);
  CHECK(script->Run(context.local()).IsEmpty());
  CHECK(message_received);
  // clear out the message listener
  CcTest::isolate()->RemoveMessageListeners(check_message_0);
}


static void check_message_1(v8::Local<v8::Message> message,
                            v8::Local<Value> data) {
  CHECK(data->IsNumber());
  CHECK_EQ(1337,
           data->Int32Value(CcTest::isolate()->GetCurrentContext()).FromJust());
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


TEST(MessageHandler1) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  CcTest::isolate()->AddMessageListener(check_message_1);
  LocalContext context;
  CompileRun("throw 1337;");
  CHECK(message_received);
  // clear out the message listener
  CcTest::isolate()->RemoveMessageListeners(check_message_1);
}


static void check_message_2(v8::Local<v8::Message> message,
                            v8::Local<Value> data) {
  LocalContext context;
  CHECK(data->IsObject());
  v8::Local<v8::Value> hidden_property =
      v8::Object::Cast(*data)
          ->GetPrivate(
              context.local(),
              v8::Private::ForApi(CcTest::isolate(), v8_str("hidden key")))
          .ToLocalChecked();
  CHECK(v8_str("hidden value")
            ->Equals(context.local(), hidden_property)
            .FromJust());
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


TEST(MessageHandler2) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  CcTest::isolate()->AddMessageListener(check_message_2);
  LocalContext context;
  v8::Local<v8::Value> error = v8::Exception::Error(v8_str("custom error"));
  v8::Object::Cast(*error)
      ->SetPrivate(context.local(),
                   v8::Private::ForApi(CcTest::isolate(), v8_str("hidden key")),
                   v8_str("hidden value"))
      .FromJust();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("error"), error)
            .FromJust());
  CompileRun("throw error;");
  CHECK(message_received);
  // clear out the message listener
  CcTest::isolate()->RemoveMessageListeners(check_message_2);
}


static void check_message_3(v8::Local<v8::Message> message,
                            v8::Local<Value> data) {
  CHECK(message->IsSharedCrossOrigin());
  CHECK(message->GetScriptOrigin().Options().IsSharedCrossOrigin());
  CHECK(message->GetScriptOrigin().Options().IsOpaque());
  CHECK_EQ(6.75, message->GetScriptOrigin()
                     .ResourceName()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  CHECK_EQ(7.40, message->GetScriptOrigin()
                     .SourceMapUrl()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  message_received = true;
}


TEST(MessageHandler3) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  isolate->AddMessageListener(check_message_3);
  LocalContext context;
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::True(isolate),
                       Local<v8::Integer>(), v8_str("7.40"), v8::True(isolate));
  v8::Local<v8::Script> script =
      Script::Compile(context.local(), v8_str("throw 'error'"), &origin)
          .ToLocalChecked();
  CHECK(script->Run(context.local()).IsEmpty());
  CHECK(message_received);
  // clear out the message listener
  isolate->RemoveMessageListeners(check_message_3);
}


static void check_message_4(v8::Local<v8::Message> message,
                            v8::Local<Value> data) {
  CHECK(!message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin()
                     .ResourceName()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  message_received = true;
}


TEST(MessageHandler4) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  isolate->AddMessageListener(check_message_4);
  LocalContext context;
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::False(isolate));
  v8::Local<v8::Script> script =
      Script::Compile(context.local(), v8_str("throw 'error'"), &origin)
          .ToLocalChecked();
  CHECK(script->Run(context.local()).IsEmpty());
  CHECK(message_received);
  // clear out the message listener
  isolate->RemoveMessageListeners(check_message_4);
}


static void check_message_5a(v8::Local<v8::Message> message,
                             v8::Local<Value> data) {
  CHECK(message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin()
                     .ResourceName()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  message_received = true;
}


static void check_message_5b(v8::Local<v8::Message> message,
                             v8::Local<Value> data) {
  CHECK(!message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin()
                     .ResourceName()
                     ->NumberValue(CcTest::isolate()->GetCurrentContext())
                     .FromJust());
  message_received = true;
}


TEST(MessageHandler5) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  isolate->AddMessageListener(check_message_5a);
  LocalContext context;
  v8::ScriptOrigin origin1 =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::True(isolate));
  v8::Local<v8::Script> script =
      Script::Compile(context.local(), v8_str("throw 'error'"), &origin1)
          .ToLocalChecked();
  CHECK(script->Run(context.local()).IsEmpty());
  CHECK(message_received);
  // clear out the message listener
  isolate->RemoveMessageListeners(check_message_5a);

  message_received = false;
  isolate->AddMessageListener(check_message_5b);
  v8::ScriptOrigin origin2 =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::False(isolate));
  script = Script::Compile(context.local(), v8_str("throw 'error'"), &origin2)
               .ToLocalChecked();
  CHECK(script->Run(context.local()).IsEmpty());
  CHECK(message_received);
  // clear out the message listener
  isolate->RemoveMessageListeners(check_message_5b);
}

namespace {

// Verifies that after throwing an exception the message object is set up in
// some particular way by calling the supplied |tester| function. The tests that
// use this purposely test only a single getter as the getter updates the cached
// state of the object which could affect the results of other functions.
void CheckMessageAttributes(std::function<void(v8::Local<v8::Context> context,
                                               v8::Local<v8::Message> message)>
                                tester) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  TryCatch try_catch(context->GetIsolate());
  CompileRun(
      R"javascript(
      (function() {
        throw new Error();
      })();
      )javascript");
  CHECK(try_catch.HasCaught());

  v8::Local<v8::Value> error = try_catch.Exception();
  v8::Local<v8::Message> message =
      v8::Exception::CreateMessage(context->GetIsolate(), error);
  CHECK(!message.IsEmpty());

  tester(context.local(), message);
}

}  // namespace

TEST(MessageGetLineNumber) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        CHECK_EQ(3, message->GetLineNumber(context).FromJust());
      });
}

TEST(MessageGetStartColumn) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        CHECK_EQ(14, message->GetStartColumn(context).FromJust());
      });
}

TEST(MessageGetEndColumn) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        CHECK_EQ(15, message->GetEndColumn(context).FromJust());
      });
}

TEST(MessageGetStartPosition) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        CHECK_EQ(35, message->GetStartPosition());
      });
}

TEST(MessageGetEndPosition) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        CHECK_EQ(36, message->GetEndPosition());
      });
}

TEST(MessageGetSourceLine) {
  CheckMessageAttributes(
      [](v8::Local<v8::Context> context, v8::Local<v8::Message> message) {
        std::string result(*v8::String::Utf8Value(
            context->GetIsolate(),
            message->GetSourceLine(context).ToLocalChecked()));
        CHECK_EQ("        throw new Error();", result);
      });
}

THREADED_TEST(GetSetProperty) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("foo"), v8_num(14))
            .FromJust());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("12"), v8_num(92))
            .FromJust());
  CHECK(context->Global()
            ->Set(context.local(), v8::Integer::New(isolate, 16), v8_num(32))
            .FromJust());
  CHECK(context->Global()
            ->Set(context.local(), v8_num(13), v8_num(56))
            .FromJust());
  Local<Value> foo = CompileRun("this.foo");
  CHECK_EQ(14, foo->Int32Value(context.local()).FromJust());
  Local<Value> twelve = CompileRun("this[12]");
  CHECK_EQ(92, twelve->Int32Value(context.local()).FromJust());
  Local<Value> sixteen = CompileRun("this[16]");
  CHECK_EQ(32, sixteen->Int32Value(context.local()).FromJust());
  Local<Value> thirteen = CompileRun("this[13]");
  CHECK_EQ(56, thirteen->Int32Value(context.local()).FromJust());
  CHECK_EQ(92, context->Global()
                   ->Get(context.local(), v8::Integer::New(isolate, 12))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(92, context->Global()
                   ->Get(context.local(), v8_str("12"))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(92, context->Global()
                   ->Get(context.local(), v8_num(12))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(32, context->Global()
                   ->Get(context.local(), v8::Integer::New(isolate, 16))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(32, context->Global()
                   ->Get(context.local(), v8_str("16"))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(32, context->Global()
                   ->Get(context.local(), v8_num(16))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(56, context->Global()
                   ->Get(context.local(), v8::Integer::New(isolate, 13))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(56, context->Global()
                   ->Get(context.local(), v8_str("13"))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(56, context->Global()
                   ->Get(context.local(), v8_num(13))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
}


THREADED_TEST(PropertyAttributes) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  // none
  Local<String> prop = v8_str("none");
  CHECK(context->Global()->Set(context.local(), prop, v8_num(7)).FromJust());
  CHECK_EQ(v8::None, context->Global()
                         ->GetPropertyAttributes(context.local(), prop)
                         .FromJust());
  // read-only
  prop = v8_str("read_only");
  context->Global()
      ->DefineOwnProperty(context.local(), prop, v8_num(7), v8::ReadOnly)
      .FromJust();
  CHECK_EQ(7, context->Global()
                  ->Get(context.local(), prop)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(v8::ReadOnly, context->Global()
                             ->GetPropertyAttributes(context.local(), prop)
                             .FromJust());
  CompileRun("read_only = 9");
  CHECK_EQ(7, context->Global()
                  ->Get(context.local(), prop)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(context->Global()->Set(context.local(), prop, v8_num(10)).FromJust());
  CHECK_EQ(7, context->Global()
                  ->Get(context.local(), prop)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  // dont-delete
  prop = v8_str("dont_delete");
  context->Global()
      ->DefineOwnProperty(context.local(), prop, v8_num(13), v8::DontDelete)
      .FromJust();
  CHECK_EQ(13, context->Global()
                   ->Get(context.local(), prop)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CompileRun("delete dont_delete");
  CHECK_EQ(13, context->Global()
                   ->Get(context.local(), prop)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(v8::DontDelete, context->Global()
                               ->GetPropertyAttributes(context.local(), prop)
                               .FromJust());
  // dont-enum
  prop = v8_str("dont_enum");
  context->Global()
      ->DefineOwnProperty(context.local(), prop, v8_num(28), v8::DontEnum)
      .FromJust();
  CHECK_EQ(v8::DontEnum, context->Global()
                             ->GetPropertyAttributes(context.local(), prop)
                             .FromJust());
  // absent
  prop = v8_str("absent");
  CHECK_EQ(v8::None, context->Global()
                         ->GetPropertyAttributes(context.local(), prop)
                         .FromJust());
  Local<Value> fake_prop = v8_num(1);
  CHECK_EQ(v8::None, context->Global()
                         ->GetPropertyAttributes(context.local(), fake_prop)
                         .FromJust());
  // exception
  TryCatch try_catch(context->GetIsolate());
  Local<Value> exception =
      CompileRun("({ toString: function() { throw 'exception';} })");
  CHECK(context->Global()
            ->GetPropertyAttributes(context.local(), exception)
            .IsNothing());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(context->GetIsolate(),
                                    try_catch.Exception());
  CHECK_EQ(0, strcmp("exception", *exception_value));
  try_catch.Reset();
}


THREADED_TEST(Array) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<v8::Array> array = v8::Array::New(context->GetIsolate());
  CHECK_EQ(0u, array->Length());
  CHECK(array->Get(context.local(), 0).ToLocalChecked()->IsUndefined());
  CHECK(!array->Has(context.local(), 0).FromJust());
  CHECK(array->Get(context.local(), 100).ToLocalChecked()->IsUndefined());
  CHECK(!array->Has(context.local(), 100).FromJust());
  CHECK(array->Set(context.local(), 2, v8_num(7)).FromJust());
  CHECK_EQ(3u, array->Length());
  CHECK(!array->Has(context.local(), 0).FromJust());
  CHECK(!array->Has(context.local(), 1).FromJust());
  CHECK(array->Has(context.local(), 2).FromJust());
  CHECK_EQ(7, array->Get(context.local(), 2)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  Local<Value> obj = CompileRun("[1, 2, 3]");
  Local<v8::Array> arr = obj.As<v8::Array>();
  CHECK_EQ(3u, arr->Length());
  CHECK_EQ(1, arr->Get(context.local(), 0)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(2, arr->Get(context.local(), 1)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(3, arr->Get(context.local(), 2)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  array = v8::Array::New(context->GetIsolate(), 27);
  CHECK_EQ(27u, array->Length());
  array = v8::Array::New(context->GetIsolate(), -27);
  CHECK_EQ(0u, array->Length());

  std::vector<Local<Value>> vector = {v8_num(1), v8_num(2), v8_num(3)};
  array = v8::Array::New(context->GetIsolate(), vector.data(), vector.size());
  CHECK_EQ(vector.size(), array->Length());
  CHECK_EQ(1, arr->Get(context.local(), 0)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(2, arr->Get(context.local(), 1)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(3, arr->Get(context.local(), 2)
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
}


void HandleF(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::EscapableHandleScope scope(args.GetIsolate());
  ApiTestFuzzer::Fuzz();
  Local<v8::Array> result = v8::Array::New(args.GetIsolate(), args.Length());
  for (int i = 0; i < args.Length(); i++) {
    CHECK(result->Set(CcTest::isolate()->GetCurrentContext(), i, args[i])
              .FromJust());
  }
  args.GetReturnValue().Set(scope.Escape(result));
}


THREADED_TEST(Vector) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("f"), v8::FunctionTemplate::New(isolate, HandleF));
  LocalContext context(nullptr, global);

  const char* fun = "f()";
  Local<v8::Array> a0 = CompileRun(fun).As<v8::Array>();
  CHECK_EQ(0u, a0->Length());

  const char* fun2 = "f(11)";
  Local<v8::Array> a1 = CompileRun(fun2).As<v8::Array>();
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(11, a1->Get(context.local(), 0)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());

  const char* fun3 = "f(12, 13)";
  Local<v8::Array> a2 = CompileRun(fun3).As<v8::Array>();
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(12, a2->Get(context.local(), 0)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(13, a2->Get(context.local(), 1)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());

  const char* fun4 = "f(14, 15, 16)";
  Local<v8::Array> a3 = CompileRun(fun4).As<v8::Array>();
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(14, a3->Get(context.local(), 0)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(15, a3->Get(context.local(), 1)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(16, a3->Get(context.local(), 2)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());

  const char* fun5 = "f(17, 18, 19, 20)";
  Local<v8::Array> a4 = CompileRun(fun5).As<v8::Array>();
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(17, a4->Get(context.local(), 0)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(18, a4->Get(context.local(), 1)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(19, a4->Get(context.local(), 2)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(20, a4->Get(context.local(), 3)
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
}


THREADED_TEST(FunctionCall) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function Foo() {"
      "  var result = [];"
      "  for (var i = 0; i < arguments.length; i++) {"
      "    result.push(arguments[i]);"
      "  }"
      "  return result;"
      "}"
      "function ReturnThisSloppy() {"
      "  return this;"
      "}"
      "function ReturnThisStrict() {"
      "  'use strict';"
      "  return this;"
      "}");
  Local<Function> Foo = Local<Function>::Cast(
      context->Global()->Get(context.local(), v8_str("Foo")).ToLocalChecked());
  Local<Function> ReturnThisSloppy = Local<Function>::Cast(
      context->Global()
          ->Get(context.local(), v8_str("ReturnThisSloppy"))
          .ToLocalChecked());
  Local<Function> ReturnThisStrict = Local<Function>::Cast(
      context->Global()
          ->Get(context.local(), v8_str("ReturnThisStrict"))
          .ToLocalChecked());

  v8::Local<Value>* args0 = nullptr;
  Local<v8::Array> a0 = Local<v8::Array>::Cast(
      Foo->Call(context.local(), Foo, 0, args0).ToLocalChecked());
  CHECK_EQ(0u, a0->Length());

  v8::Local<Value> args1[] = {v8_num(1.1)};
  Local<v8::Array> a1 = Local<v8::Array>::Cast(
      Foo->Call(context.local(), Foo, 1, args1).ToLocalChecked());
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(1.1, a1->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args2[] = {v8_num(2.2), v8_num(3.3)};
  Local<v8::Array> a2 = Local<v8::Array>::Cast(
      Foo->Call(context.local(), Foo, 2, args2).ToLocalChecked());
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(2.2, a2->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(3.3, a2->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args3[] = {v8_num(4.4), v8_num(5.5), v8_num(6.6)};
  Local<v8::Array> a3 = Local<v8::Array>::Cast(
      Foo->Call(context.local(), Foo, 3, args3).ToLocalChecked());
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(4.4, a3->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(5.5, a3->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(6.6, a3->Get(context.local(), v8::Integer::New(isolate, 2))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args4[] = {v8_num(7.7), v8_num(8.8), v8_num(9.9),
                              v8_num(10.11)};
  Local<v8::Array> a4 = Local<v8::Array>::Cast(
      Foo->Call(context.local(), Foo, 4, args4).ToLocalChecked());
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(7.7, a4->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(8.8, a4->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(9.9, a4->Get(context.local(), v8::Integer::New(isolate, 2))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(10.11, a4->Get(context.local(), v8::Integer::New(isolate, 3))
                      .ToLocalChecked()
                      ->NumberValue(context.local())
                      .FromJust());

  Local<v8::Value> r1 =
      ReturnThisSloppy
          ->Call(context.local(), v8::Undefined(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r1->StrictEquals(context->Global()));
  Local<v8::Value> r2 =
      ReturnThisSloppy->Call(context.local(), v8::Null(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r2->StrictEquals(context->Global()));
  Local<v8::Value> r3 =
      ReturnThisSloppy->Call(context.local(), v8_num(42), 0, nullptr)
          .ToLocalChecked();
  CHECK(r3->IsNumberObject());
  CHECK_EQ(42.0, r3.As<v8::NumberObject>()->ValueOf());
  Local<v8::Value> r4 =
      ReturnThisSloppy->Call(context.local(), v8_str("hello"), 0, nullptr)
          .ToLocalChecked();
  CHECK(r4->IsStringObject());
  CHECK(r4.As<v8::StringObject>()->ValueOf()->StrictEquals(v8_str("hello")));
  Local<v8::Value> r5 =
      ReturnThisSloppy->Call(context.local(), v8::True(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r5->IsBooleanObject());
  CHECK(r5.As<v8::BooleanObject>()->ValueOf());

  Local<v8::Value> r6 =
      ReturnThisStrict
          ->Call(context.local(), v8::Undefined(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r6->IsUndefined());
  Local<v8::Value> r7 =
      ReturnThisStrict->Call(context.local(), v8::Null(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r7->IsNull());
  Local<v8::Value> r8 =
      ReturnThisStrict->Call(context.local(), v8_num(42), 0, nullptr)
          .ToLocalChecked();
  CHECK(r8->StrictEquals(v8_num(42)));
  Local<v8::Value> r9 =
      ReturnThisStrict->Call(context.local(), v8_str("hello"), 0, nullptr)
          .ToLocalChecked();
  CHECK(r9->StrictEquals(v8_str("hello")));
  Local<v8::Value> r10 =
      ReturnThisStrict->Call(context.local(), v8::True(isolate), 0, nullptr)
          .ToLocalChecked();
  CHECK(r10->StrictEquals(v8::True(isolate)));
}


THREADED_TEST(ConstructCall) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function Foo() {"
      "  var result = [];"
      "  for (var i = 0; i < arguments.length; i++) {"
      "    result.push(arguments[i]);"
      "  }"
      "  return result;"
      "}");
  Local<Function> Foo = Local<Function>::Cast(
      context->Global()->Get(context.local(), v8_str("Foo")).ToLocalChecked());

  v8::Local<Value>* args0 = nullptr;
  Local<v8::Array> a0 = Local<v8::Array>::Cast(
      Foo->NewInstance(context.local(), 0, args0).ToLocalChecked());
  CHECK_EQ(0u, a0->Length());

  v8::Local<Value> args1[] = {v8_num(1.1)};
  Local<v8::Array> a1 = Local<v8::Array>::Cast(
      Foo->NewInstance(context.local(), 1, args1).ToLocalChecked());
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(1.1, a1->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args2[] = {v8_num(2.2), v8_num(3.3)};
  Local<v8::Array> a2 = Local<v8::Array>::Cast(
      Foo->NewInstance(context.local(), 2, args2).ToLocalChecked());
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(2.2, a2->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(3.3, a2->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args3[] = {v8_num(4.4), v8_num(5.5), v8_num(6.6)};
  Local<v8::Array> a3 = Local<v8::Array>::Cast(
      Foo->NewInstance(context.local(), 3, args3).ToLocalChecked());
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(4.4, a3->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(5.5, a3->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(6.6, a3->Get(context.local(), v8::Integer::New(isolate, 2))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());

  v8::Local<Value> args4[] = {v8_num(7.7), v8_num(8.8), v8_num(9.9),
                              v8_num(10.11)};
  Local<v8::Array> a4 = Local<v8::Array>::Cast(
      Foo->NewInstance(context.local(), 4, args4).ToLocalChecked());
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(7.7, a4->Get(context.local(), v8::Integer::New(isolate, 0))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(8.8, a4->Get(context.local(), v8::Integer::New(isolate, 1))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(9.9, a4->Get(context.local(), v8::Integer::New(isolate, 2))
                    .ToLocalChecked()
                    ->NumberValue(context.local())
                    .FromJust());
  CHECK_EQ(10.11, a4->Get(context.local(), v8::Integer::New(isolate, 3))
                      .ToLocalChecked()
                      ->NumberValue(context.local())
                      .FromJust());
}


THREADED_TEST(ConversionNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  // Very large number.
  CompileRun("var obj = Math.pow(2,32) * 1237;");
  Local<Value> obj =
      env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(5312874545152.0,
           obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(0, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(0, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Large number.
  CompileRun("var obj = -1234567890123;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(-1234567890123.0,
           obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(-1912276171, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(2382691125, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Small positive integer.
  CompileRun("var obj = 42;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(42.0, obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(42, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(42, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Negative integer.
  CompileRun("var obj = -37;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(-37.0, obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(-37, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(4294967259, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Positive non-int32 integer.
  CompileRun("var obj = 0x81234567;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(2166572391.0, obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(-2128394905, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(2166572391, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Fraction.
  CompileRun("var obj = 42.3;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(42.3, obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(42, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(42, obj->ToUint32(env.local()).ToLocalChecked()->Value());
  // Large negative fraction.
  CompileRun("var obj = -5726623061.75;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK_EQ(-5726623061.75,
           obj->ToNumber(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(-1431655765, obj->ToInt32(env.local()).ToLocalChecked()->Value());
  CHECK_EQ(2863311531, obj->ToUint32(env.local()).ToLocalChecked()->Value());
}


THREADED_TEST(isNumberType) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  // Very large number.
  CompileRun("var obj = Math.pow(2,32) * 1237;");
  Local<Value> obj =
      env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Large negative number.
  CompileRun("var obj = -1234567890123;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Small positive integer.
  CompileRun("var obj = 42;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(obj->IsInt32());
  CHECK(obj->IsUint32());
  // Negative integer.
  CompileRun("var obj = -37;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Positive non-int32 integer.
  CompileRun("var obj = 0x81234567;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(obj->IsUint32());
  // Fraction.
  CompileRun("var obj = 42.3;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Large negative fraction.
  CompileRun("var obj = -5726623061.75;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Positive zero
  CompileRun("var obj = 0.0;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(obj->IsInt32());
  CHECK(obj->IsUint32());
  // Negative zero
  CompileRun("var obj = -0.0;");
  obj = env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
}

THREADED_TEST(IntegerType) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<Value> result;

  // Small positive integer
  result = CompileRun("42;");
  CHECK(result->IsNumber());
  CHECK_EQ(42, result.As<v8::Integer>()->Value());
  // Small negative integer
  result = CompileRun("-42;");
  CHECK(result->IsNumber());
  CHECK_EQ(-42, result.As<v8::Integer>()->Value());
  // Positive non-int32 integer
  result = CompileRun("1099511627776;");
  CHECK(result->IsNumber());
  CHECK_EQ(1099511627776, result.As<v8::Integer>()->Value());
  // Negative non-int32 integer
  result = CompileRun("-1099511627776;");
  CHECK(result->IsNumber());
  CHECK_EQ(-1099511627776, result.As<v8::Integer>()->Value());
  // Positive non-integer
  result = CompileRun("3.14;");
  CHECK(result->IsNumber());
  CHECK_EQ(3, result.As<v8::Integer>()->Value());
  // Negative non-integer
  result = CompileRun("-3.14;");
  CHECK(result->IsNumber());
  CHECK_EQ(-3, result.As<v8::Integer>()->Value());
}

static void CheckUncle(v8::Isolate* isolate, v8::TryCatch* try_catch) {
  CHECK(try_catch->HasCaught());
  String::Utf8Value str_value(isolate, try_catch->Exception());
  CHECK_EQ(0, strcmp(*str_value, "uncle?"));
  try_catch->Reset();
}

THREADED_TEST(ConversionException) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function TestClass() { };"
      "TestClass.prototype.toString = function () { throw 'uncle?'; };"
      "var obj = new TestClass();");
  Local<Value> obj =
      env->Global()->Get(env.local(), v8_str("obj")).ToLocalChecked();

  v8::TryCatch try_catch(isolate);

  CHECK(obj->ToString(env.local()).IsEmpty());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->ToNumber(env.local()).IsEmpty());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->ToInteger(env.local()).IsEmpty());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->ToUint32(env.local()).IsEmpty());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->ToInt32(env.local()).IsEmpty());
  CheckUncle(isolate, &try_catch);

  CHECK(v8::Undefined(isolate)->ToObject(env.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(obj->Int32Value(env.local()).IsNothing());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->Uint32Value(env.local()).IsNothing());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->NumberValue(env.local()).IsNothing());
  CheckUncle(isolate, &try_catch);

  CHECK(obj->IntegerValue(env.local()).IsNothing());
  CheckUncle(isolate, &try_catch);
}


void ThrowFromC(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetIsolate()->ThrowException(v8_str("konto"));
}


void CCatcher(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 1) {
    args.GetReturnValue().Set(false);
    return;
  }
  v8::HandleScope scope(args.GetIsolate());
  v8::TryCatch try_catch(args.GetIsolate());
  Local<Value> result =
      CompileRun(args[0]
                     ->ToString(args.GetIsolate()->GetCurrentContext())
                     .ToLocalChecked());
  CHECK(!try_catch.HasCaught() || result.IsEmpty());
  args.GetReturnValue().Set(try_catch.HasCaught());
}


THREADED_TEST(APICatch) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(nullptr, templ);
  CompileRun(
      "var thrown = false;"
      "try {"
      "  ThrowFromC();"
      "} catch (e) {"
      "  thrown = true;"
      "}");
  Local<Value> thrown = context->Global()
                            ->Get(context.local(), v8_str("thrown"))
                            .ToLocalChecked();
  CHECK(thrown->BooleanValue(isolate));
}


THREADED_TEST(APIThrowTryCatch) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(nullptr, templ);
  v8::TryCatch try_catch(isolate);
  CompileRun("ThrowFromC();");
  CHECK(try_catch.HasCaught());
}


// Test that a try-finally block doesn't shadow a try-catch block
// when setting up an external handler.
//
// BUG(271): Some of the exception propagation does not work on the
// ARM simulator because the simulator separates the C++ stack and the
// JS stack.  This test therefore fails on the simulator.  The test is
// not threaded to allow the threading tests to run on the simulator.
TEST(TryCatchInTryFinally) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("CCatcher"), v8::FunctionTemplate::New(isolate, CCatcher));
  LocalContext context(nullptr, templ);
  Local<Value> result = CompileRun(
      "try {"
      "  try {"
      "    CCatcher('throw 7;');"
      "  } finally {"
      "  }"
      "} catch (e) {"
      "}");
  CHECK(result->IsTrue());
}


static void check_custom_error_tostring(v8::Local<v8::Message> message,
                                        v8::Local<v8::Value> data) {
  const char* uncaught_error = "Uncaught MyError toString";
  CHECK(message->Get()
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8_str(uncaught_error))
            .FromJust());
}


TEST(CustomErrorToString) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  context->GetIsolate()->AddMessageListener(check_custom_error_tostring);
  CompileRun(
      "function MyError(name, message) {                   "
      "  this.name = name;                                 "
      "  this.message = message;                           "
      "}                                                   "
      "MyError.prototype = Object.create(Error.prototype); "
      "MyError.prototype.toString = function() {           "
      "  return 'MyError toString';                        "
      "};                                                  "
      "throw new MyError('my name', 'my message');         ");
  context->GetIsolate()->RemoveMessageListeners(check_custom_error_tostring);
}


static void check_custom_error_message(v8::Local<v8::Message> message,
                                       v8::Local<v8::Value> data) {
  const char* uncaught_error = "Uncaught MyError: my message";
  printf("%s\n", *v8::String::Utf8Value(CcTest::isolate(), message->Get()));
  CHECK(message->Get()
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8_str(uncaught_error))
            .FromJust());
}


TEST(CustomErrorMessage) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  context->GetIsolate()->AddMessageListener(check_custom_error_message);

  // Handlebars.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "MyError.prototype = new Error();                    "
      "throw new MyError('my message');                    ");

  // Closure.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "inherits = function(childCtor, parentCtor) {        "
      "    function tempCtor() {};                         "
      "    tempCtor.prototype = parentCtor.prototype;      "
      "    childCtor.superClass_ = parentCtor.prototype;   "
      "    childCtor.prototype = new tempCtor();           "
      "    childCtor.prototype.constructor = childCtor;    "
      "};                                                  "
      "inherits(MyError, Error);                           "
      "throw new MyError('my message');                    ");

  // Object.create.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "MyError.prototype = Object.create(Error.prototype); "
      "throw new MyError('my message');                    ");

  context->GetIsolate()->RemoveMessageListeners(check_custom_error_message);
}


static void check_custom_rethrowing_message(v8::Local<v8::Message> message,
                                            v8::Local<v8::Value> data) {
  CHECK(data->IsExternal());
  int* callcount = static_cast<int*>(data.As<v8::External>()->Value());
  ++*callcount;

  const char* uncaught_error = "Uncaught exception";
  CHECK(message->Get()
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8_str(uncaught_error))
            .FromJust());
  // Test that compiling code inside a message handler works.
  CHECK(CompileRunChecked(CcTest::isolate(), "(function(a) { return a; })(42)")
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8::Integer::NewFromUnsigned(CcTest::isolate(), 42))
            .FromJust());
}


TEST(CustomErrorRethrowsOnToString) {
  int callcount = 0;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  context->GetIsolate()->AddMessageListener(
      check_custom_rethrowing_message, v8::External::New(isolate, &callcount));

  CompileRun(
      "var e = { toString: function() { throw e; } };"
      "try { throw e; } finally {}");

  CHECK_EQ(callcount, 1);
  context->GetIsolate()->RemoveMessageListeners(
      check_custom_rethrowing_message);
}

TEST(CustomErrorRethrowsOnToStringInsideVerboseTryCatch) {
  int callcount = 0;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);
  try_catch.SetVerbose(true);
  context->GetIsolate()->AddMessageListener(
      check_custom_rethrowing_message, v8::External::New(isolate, &callcount));

  CompileRun(
      "var e = { toString: function() { throw e; } };"
      "try { throw e; } finally {}");

  CHECK_EQ(callcount, 1);
  context->GetIsolate()->RemoveMessageListeners(
      check_custom_rethrowing_message);
}


static void receive_message(v8::Local<v8::Message> message,
                            v8::Local<v8::Value> data) {
  message->Get();
  message_received = true;
}


TEST(APIThrowMessage) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  isolate->AddMessageListener(receive_message);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(nullptr, templ);
  CompileRun("ThrowFromC();");
  CHECK(message_received);
  isolate->RemoveMessageListeners(receive_message);
}


TEST(APIThrowMessageAndVerboseTryCatch) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  isolate->AddMessageListener(receive_message);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(nullptr, templ);
  v8::TryCatch try_catch(isolate);
  try_catch.SetVerbose(true);
  Local<Value> result = CompileRun("ThrowFromC();");
  CHECK(try_catch.HasCaught());
  CHECK(result.IsEmpty());
  CHECK(message_received);
  isolate->RemoveMessageListeners(receive_message);
}


TEST(APIStackOverflowAndVerboseTryCatch) {
  message_received = false;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  context->GetIsolate()->AddMessageListener(receive_message);
  v8::TryCatch try_catch(context->GetIsolate());
  try_catch.SetVerbose(true);
  Local<Value> result = CompileRun("function foo() { foo(); } foo();");
  CHECK(try_catch.HasCaught());
  CHECK(result.IsEmpty());
  CHECK(message_received);
  context->GetIsolate()->RemoveMessageListeners(receive_message);
}


THREADED_TEST(ExternalScriptException) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(nullptr, templ);

  v8::TryCatch try_catch(isolate);
  Local<Value> result = CompileRun("ThrowFromC(); throw 'panama';");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(isolate, try_catch.Exception());
  CHECK_EQ(0, strcmp("konto", *exception_value));
}


void CThrowCountDown(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(4, args.Length());
  v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
  int count = args[0]->Int32Value(context).FromJust();
  int cInterval = args[2]->Int32Value(context).FromJust();
  if (count == 0) {
    args.GetIsolate()->ThrowException(v8_str("FromC"));
    return;
  } else {
    Local<v8::Object> global = context->Global();
    Local<Value> fun =
        global->Get(context, v8_str("JSThrowCountDown")).ToLocalChecked();
    v8::Local<Value> argv[] = {v8_num(count - 1), args[1], args[2], args[3]};
    if (count % cInterval == 0) {
      v8::TryCatch try_catch(args.GetIsolate());
      Local<Value> result = fun.As<Function>()
                                ->Call(context, global, 4, argv)
                                .FromMaybe(Local<Value>());
      int expected = args[3]->Int32Value(context).FromJust();
      if (try_catch.HasCaught()) {
        CHECK_EQ(expected, count);
        CHECK(result.IsEmpty());
        CHECK(!CcTest::i_isolate()->has_scheduled_exception());
      } else {
        CHECK_NE(expected, count);
      }
      args.GetReturnValue().Set(result);
      return;
    } else {
      args.GetReturnValue().Set(fun.As<Function>()
                                    ->Call(context, global, 4, argv)
                                    .FromMaybe(v8::Local<v8::Value>()));
      return;
    }
  }
}


void JSCheck(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(3, args.Length());
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  bool equality = args[0]->BooleanValue(isolate);
  int count = args[1]->Int32Value(context).FromJust();
  int expected = args[2]->Int32Value(context).FromJust();
  if (equality) {
    CHECK_EQ(count, expected);
  } else {
    CHECK_NE(count, expected);
  }
}


THREADED_TEST(EvalInTryFinally) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  CompileRun(
      "(function() {"
      "  try {"
      "    eval('asldkf (*&^&*^');"
      "  } finally {"
      "    return;"
      "  }"
      "})()");
  CHECK(!try_catch.HasCaught());
}


// This test works by making a stack of alternating JavaScript and C
// activations.  These activations set up exception handlers with regular
// intervals, one interval for C activations and another for JavaScript
// activations.  When enough activations have been created an exception is
// thrown and we check that the right activation catches the exception and that
// no other activations do.  The right activation is always the topmost one with
// a handler, regardless of whether it is in JavaScript or C.
//
// The notation used to describe a test case looks like this:
//
//    *JS[4] *C[3] @JS[2] C[1] JS[0]
//
// Each entry is an activation, either JS or C.  The index is the count at that
// level.  Stars identify activations with exception handlers, the @ identifies
// the exception handler that should catch the exception.
//
// BUG(271): Some of the exception propagation does not work on the
// ARM simulator because the simulator separates the C++ stack and the
// JS stack.  This test therefore fails on the simulator.  The test is
// not threaded to allow the threading tests to run on the simulator.
TEST(ExceptionOrder) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("check"), v8::FunctionTemplate::New(isolate, JSCheck));
  templ->Set(v8_str("CThrowCountDown"),
             v8::FunctionTemplate::New(isolate, CThrowCountDown));
  LocalContext context(nullptr, templ);
  CompileRun(
      "function JSThrowCountDown(count, jsInterval, cInterval, expected) {"
      "  if (count == 0) throw 'FromJS';"
      "  if (count % jsInterval == 0) {"
      "    try {"
      "      var value = CThrowCountDown(count - 1,"
      "                                  jsInterval,"
      "                                  cInterval,"
      "                                  expected);"
      "      check(false, count, expected);"
      "      return value;"
      "    } catch (e) {"
      "      check(true, count, expected);"
      "    }"
      "  } else {"
      "    return CThrowCountDown(count - 1, jsInterval, cInterval, expected);"
      "  }"
      "}");
  Local<Function> fun = Local<Function>::Cast(
      context->Global()
          ->Get(context.local(), v8_str("JSThrowCountDown"))
          .ToLocalChecked());

  const int argc = 4;
  //                             count      jsInterval cInterval  expected

  // *JS[4] *C[3] @JS[2] C[1] JS[0]
  v8::Local<Value> a0[argc] = {v8_num(4), v8_num(2), v8_num(3), v8_num(2)};
  fun->Call(context.local(), fun, argc, a0).ToLocalChecked();

  // JS[5] *C[4] JS[3] @C[2] JS[1] C[0]
  v8::Local<Value> a1[argc] = {v8_num(5), v8_num(6), v8_num(1), v8_num(2)};
  fun->Call(context.local(), fun, argc, a1).ToLocalChecked();

  // JS[6] @C[5] JS[4] C[3] JS[2] C[1] JS[0]
  v8::Local<Value> a2[argc] = {v8_num(6), v8_num(7), v8_num(5), v8_num(5)};
  fun->Call(context.local(), fun, argc, a2).ToLocalChecked();

  // @JS[6] C[5] JS[4] C[3] JS[2] C[1] JS[0]
  v8::Local<Value> a3[argc] = {v8_num(6), v8_num(6), v8_num(7), v8_num(6)};
  fun->Call(context.local(), fun, argc, a3).ToLocalChecked();

  // JS[6] *C[5] @JS[4] C[3] JS[2] C[1] JS[0]
  v8::Local<Value> a4[argc] = {v8_num(6), v8_num(4), v8_num(5), v8_num(4)};
  fun->Call(context.local(), fun, argc, a4).ToLocalChecked();

  // JS[6] C[5] *JS[4] @C[3] JS[2] C[1] JS[0]
  v8::Local<Value> a5[argc] = {v8_num(6), v8_num(4), v8_num(3), v8_num(3)};
  fun->Call(context.local(), fun, argc, a5).ToLocalChecked();
}


void ThrowValue(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(1, args.Length());
  args.GetIsolate()->ThrowException(args[0]);
}


THREADED_TEST(ThrowValues) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("Throw"), v8::FunctionTemplate::New(isolate, ThrowValue));
  LocalContext context(nullptr, templ);
  v8::Local<v8::Array> result = v8::Local<v8::Array>::Cast(
      CompileRun("function Run(obj) {"
                 "  try {"
                 "    Throw(obj);"
                 "  } catch (e) {"
                 "    return e;"
                 "  }"
                 "  return 'no exception';"
                 "}"
                 "[Run('str'), Run(1), Run(0), Run(null), Run(void 0)];"));
  CHECK_EQ(5u, result->Length());
  CHECK(result->Get(context.local(), v8::Integer::New(isolate, 0))
            .ToLocalChecked()
            ->IsString());
  CHECK(result->Get(context.local(), v8::Integer::New(isolate, 1))
            .ToLocalChecked()
            ->IsNumber());
  CHECK_EQ(1, result->Get(context.local(), v8::Integer::New(isolate, 1))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(result->Get(context.local(), v8::Integer::New(isolate, 2))
            .ToLocalChecked()
            ->IsNumber());
  CHECK_EQ(0, result->Get(context.local(), v8::Integer::New(isolate, 2))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(result->Get(context.local(), v8::Integer::New(isolate, 3))
            .ToLocalChecked()
            ->IsNull());
  CHECK(result->Get(context.local(), v8::Integer::New(isolate, 4))
            .ToLocalChecked()
            ->IsUndefined());
}


THREADED_TEST(CatchZero) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  CHECK(!try_catch.HasCaught());
  CompileRun("throw 10");
  CHECK(try_catch.HasCaught());
  CHECK_EQ(10, try_catch.Exception()->Int32Value(context.local()).FromJust());
  try_catch.Reset();
  CHECK(!try_catch.HasCaught());
  CompileRun("throw 0");
  CHECK(try_catch.HasCaught());
  CHECK_EQ(0, try_catch.Exception()->Int32Value(context.local()).FromJust());
}


THREADED_TEST(CatchExceptionFromWith) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  CHECK(!try_catch.HasCaught());
  CompileRun("var o = {}; with (o) { throw 42; }");
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(TryCatchAndFinallyHidingException) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  CHECK(!try_catch.HasCaught());
  CompileRun("function f(k) { try { this[k]; } finally { return 0; } };");
  CompileRun("f({toString: function() { throw 42; }});");
  CHECK(!try_catch.HasCaught());
}


void WithTryCatch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::TryCatch try_catch(args.GetIsolate());
}


THREADED_TEST(TryCatchAndFinally) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("native_with_try_catch"),
                  v8::FunctionTemplate::New(isolate, WithTryCatch)
                      ->GetFunction(context.local())
                      .ToLocalChecked())
            .FromJust());
  v8::TryCatch try_catch(isolate);
  CHECK(!try_catch.HasCaught());
  CompileRun(
      "try {\n"
      "  throw new Error('a');\n"
      "} finally {\n"
      "  native_with_try_catch();\n"
      "}\n");
  CHECK(try_catch.HasCaught());
}


static void TryCatchNested1Helper(int depth) {
  if (depth > 0) {
    v8::TryCatch try_catch(CcTest::isolate());
    try_catch.SetVerbose(true);
    TryCatchNested1Helper(depth - 1);
    CHECK(try_catch.HasCaught());
    try_catch.ReThrow();
  } else {
    CcTest::isolate()->ThrowException(v8_str("E1"));
  }
}


static void TryCatchNested2Helper(int depth) {
  if (depth > 0) {
    v8::TryCatch try_catch(CcTest::isolate());
    try_catch.SetVerbose(true);
    TryCatchNested2Helper(depth - 1);
    CHECK(try_catch.HasCaught());
    try_catch.ReThrow();
  } else {
    CompileRun("throw 'E2';");
  }
}


TEST(TryCatchNested) {
  v8::V8::Initialize();
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  {
    // Test nested try-catch with a native throw in the end.
    v8::TryCatch try_catch(context->GetIsolate());
    TryCatchNested1Helper(5);
    CHECK(try_catch.HasCaught());
    CHECK_EQ(0, strcmp(*v8::String::Utf8Value(context->GetIsolate(),
                                              try_catch.Exception()),
                       "E1"));
  }

  {
    // Test nested try-catch with a JavaScript throw in the end.
    v8::TryCatch try_catch(context->GetIsolate());
    TryCatchNested2Helper(5);
    CHECK(try_catch.HasCaught());
    CHECK_EQ(0, strcmp(*v8::String::Utf8Value(context->GetIsolate(),
                                              try_catch.Exception()),
                       "E2"));
  }
}


void TryCatchMixedNestingCheck(v8::TryCatch* try_catch) {
  CHECK(try_catch->HasCaught());
  Local<Message> message = try_catch->Message();
  Local<Value> resource = message->GetScriptOrigin().ResourceName();
  CHECK_EQ(
      0, strcmp(*v8::String::Utf8Value(CcTest::isolate(), resource), "inner"));
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(CcTest::isolate(), message->Get()),
                     "Uncaught Error: a"));
  CHECK_EQ(1, message->GetLineNumber(CcTest::isolate()->GetCurrentContext())
                  .FromJust());
  CHECK_EQ(0, message->GetStartColumn(CcTest::isolate()->GetCurrentContext())
                  .FromJust());
}


void TryCatchMixedNestingHelper(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch(args.GetIsolate());
  CompileRunWithOrigin("throw new Error('a');\n", "inner", 0, 0);
  CHECK(try_catch.HasCaught());
  TryCatchMixedNestingCheck(&try_catch);
  try_catch.ReThrow();
}


// This test ensures that an outer TryCatch in the following situation:
//   C++/TryCatch -> JS -> C++/TryCatch -> JS w/ SyntaxError
// does not clobber the Message object generated for the inner TryCatch.
// This exercises the ability of TryCatch.ReThrow() to restore the
// inner pending Message before throwing the exception again.
TEST(TryCatchMixedNesting) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchMixedNestingHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchMixedNestingHelper));
  LocalContext context(nullptr, templ);
  CompileRunWithOrigin("TryCatchMixedNestingHelper();\n", "outer", 1, 1);
  TryCatchMixedNestingCheck(&try_catch);
}


void TryCatchNativeHelper(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch(args.GetIsolate());
  args.GetIsolate()->ThrowException(v8_str("boom"));
  CHECK(try_catch.HasCaught());
}


TEST(TryCatchNative) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchNativeHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchNativeHelper));
  LocalContext context(nullptr, templ);
  CompileRun("TryCatchNativeHelper();");
  CHECK(!try_catch.HasCaught());
}


void TryCatchNativeResetHelper(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch(args.GetIsolate());
  args.GetIsolate()->ThrowException(v8_str("boom"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(!try_catch.HasCaught());
}


TEST(TryCatchNativeReset) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchNativeResetHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchNativeResetHelper));
  LocalContext context(nullptr, templ);
  CompileRun("TryCatchNativeResetHelper();");
  CHECK(!try_catch.HasCaught());
}


THREADED_TEST(Equality) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(context->GetIsolate());
  // Check that equality works at all before relying on CHECK_EQ
  CHECK(v8_str("a")->Equals(context.local(), v8_str("a")).FromJust());
  CHECK(!v8_str("a")->Equals(context.local(), v8_str("b")).FromJust());

  CHECK(v8_str("a")->Equals(context.local(), v8_str("a")).FromJust());
  CHECK(!v8_str("a")->Equals(context.local(), v8_str("b")).FromJust());
  CHECK(v8_num(1)->Equals(context.local(), v8_num(1)).FromJust());
  CHECK(v8_num(1.00)->Equals(context.local(), v8_num(1)).FromJust());
  CHECK(!v8_num(1)->Equals(context.local(), v8_num(2)).FromJust());

  // Assume String is not internalized.
  CHECK(v8_str("a")->StrictEquals(v8_str("a")));
  CHECK(!v8_str("a")->StrictEquals(v8_str("b")));
  CHECK(!v8_str("5")->StrictEquals(v8_num(5)));
  CHECK(v8_num(1)->StrictEquals(v8_num(1)));
  CHECK(!v8_num(1)->StrictEquals(v8_num(2)));
  CHECK(v8_num(0.0)->StrictEquals(v8_num(-0.0)));
  Local<Value> not_a_number = v8_num(std::numeric_limits<double>::quiet_NaN());
  CHECK(!not_a_number->StrictEquals(not_a_number));
  CHECK(v8::False(isolate)->StrictEquals(v8::False(isolate)));
  CHECK(!v8::False(isolate)->StrictEquals(v8::Undefined(isolate)));

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Persistent<v8::Object> alias(isolate, obj);
  CHECK(v8::Local<v8::Object>::New(isolate, alias)->StrictEquals(obj));
  alias.Reset();

  CHECK(v8_str("a")->SameValue(v8_str("a")));
  CHECK(!v8_str("a")->SameValue(v8_str("b")));
  CHECK(!v8_str("5")->SameValue(v8_num(5)));
  CHECK(v8_num(1)->SameValue(v8_num(1)));
  CHECK(!v8_num(1)->SameValue(v8_num(2)));
  CHECK(!v8_num(0.0)->SameValue(v8_num(-0.0)));
  CHECK(not_a_number->SameValue(not_a_number));
  CHECK(v8::False(isolate)->SameValue(v8::False(isolate)));
  CHECK(!v8::False(isolate)->SameValue(v8::Undefined(isolate)));
}

THREADED_TEST(TypeOf) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(context->GetIsolate());

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  Local<v8::Function> fun = t1->GetFunction(context.local()).ToLocalChecked();

  CHECK(v8::Undefined(isolate)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("undefined"))
            .FromJust());
  CHECK(v8::Null(isolate)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("object"))
            .FromJust());
  CHECK(v8_str("str")
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("string"))
            .FromJust());
  CHECK(v8_num(0.0)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("number"))
            .FromJust());
  CHECK(v8_num(1)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("number"))
            .FromJust());
  CHECK(v8::Object::New(isolate)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("object"))
            .FromJust());
  CHECK(v8::Boolean::New(isolate, true)
            ->TypeOf(isolate)
            ->Equals(context.local(), v8_str("boolean"))
            .FromJust());
  CHECK(fun->TypeOf(isolate)
            ->Equals(context.local(), v8_str("function"))
            .FromJust());
}

THREADED_TEST(InstanceOf) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  CompileRun(
      "var A = {};"
      "var B = {};"
      "var C = {};"
      "B.__proto__ = A;"
      "C.__proto__ = B;"
      "function F() {}"
      "F.prototype = A;"
      "var G = { [Symbol.hasInstance] : null};"
      "var H = { [Symbol.hasInstance] : () => { throw new Error(); } };"
      "var J = { [Symbol.hasInstance] : () => true };"
      "class K {}"
      "var D = new K;"
      "class L extends K {}"
      "var E = new L");

  v8::Local<v8::Object> f = v8::Local<v8::Object>::Cast(CompileRun("F"));
  v8::Local<v8::Object> g = v8::Local<v8::Object>::Cast(CompileRun("G"));
  v8::Local<v8::Object> h = v8::Local<v8::Object>::Cast(CompileRun("H"));
  v8::Local<v8::Object> j = v8::Local<v8::Object>::Cast(CompileRun("J"));
  v8::Local<v8::Object> k = v8::Local<v8::Object>::Cast(CompileRun("K"));
  v8::Local<v8::Object> l = v8::Local<v8::Object>::Cast(CompileRun("L"));
  v8::Local<v8::Value> a = v8::Local<v8::Value>::Cast(CompileRun("A"));
  v8::Local<v8::Value> b = v8::Local<v8::Value>::Cast(CompileRun("B"));
  v8::Local<v8::Value> c = v8::Local<v8::Value>::Cast(CompileRun("C"));
  v8::Local<v8::Value> d = v8::Local<v8::Value>::Cast(CompileRun("D"));
  v8::Local<v8::Value> e = v8::Local<v8::Value>::Cast(CompileRun("E"));

  v8::TryCatch try_catch(env->GetIsolate());
  CHECK(!a->InstanceOf(env.local(), f).ToChecked());
  CHECK(b->InstanceOf(env.local(), f).ToChecked());
  CHECK(c->InstanceOf(env.local(), f).ToChecked());
  CHECK(!d->InstanceOf(env.local(), f).ToChecked());
  CHECK(!e->InstanceOf(env.local(), f).ToChecked());
  CHECK(!try_catch.HasCaught());

  CHECK(a->InstanceOf(env.local(), g).IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(b->InstanceOf(env.local(), h).IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(v8_num(1)->InstanceOf(env.local(), j).ToChecked());
  CHECK(!try_catch.HasCaught());

  CHECK(d->InstanceOf(env.local(), k).ToChecked());
  CHECK(e->InstanceOf(env.local(), k).ToChecked());
  CHECK(!d->InstanceOf(env.local(), l).ToChecked());
  CHECK(e->InstanceOf(env.local(), l).ToChecked());
  CHECK(!try_catch.HasCaught());
}

THREADED_TEST(MultiRun) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<Script> script = v8_compile("x");
  for (int i = 0; i < 10; i++) {
    script->Run(context.local()).IsEmpty();
  }
}


static void GetXValue(Local<Name> name,
                      const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CHECK(info.Data()
            ->Equals(CcTest::isolate()->GetCurrentContext(), v8_str("donut"))
            .FromJust());
  CHECK(name->Equals(CcTest::isolate()->GetCurrentContext(), v8_str("x"))
            .FromJust());
  info.GetReturnValue().Set(name);
}


THREADED_TEST(SimplePropertyRead) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, nullptr, v8_str("donut"));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  Local<Script> script = v8_compile("obj.x");
  for (int i = 0; i < 10; i++) {
    Local<Value> result = script->Run(context.local()).ToLocalChecked();
    CHECK(result->Equals(context.local(), v8_str("x")).FromJust());
  }
}


THREADED_TEST(DefinePropertyOnAPIAccessor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, nullptr, v8_str("donut"));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  // Uses getOwnPropertyDescriptor to check the configurable status
  Local<Script> script_desc = v8_compile(
      "var prop = Object.getOwnPropertyDescriptor( "
      "obj, 'x');"
      "prop.configurable;");
  Local<Value> result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(result->BooleanValue(isolate));

  // Redefine get - but still configurable
  Local<Script> script_define = v8_compile(
      "var desc = { get: function(){return 42; },"
      "            configurable: true };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run(context.local()).ToLocalChecked();
  CHECK(result->Equals(context.local(), v8_num(42)).FromJust());

  // Check that the accessor is still configurable
  result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(result->BooleanValue(isolate));

  // Redefine to a non-configurable
  script_define = v8_compile(
      "var desc = { get: function(){return 43; },"
      "             configurable: false };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run(context.local()).ToLocalChecked();
  CHECK(result->Equals(context.local(), v8_num(43)).FromJust());
  result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(!result->BooleanValue(isolate));

  // Make sure that it is not possible to redefine again
  v8::TryCatch try_catch(isolate);
  CHECK(script_define->Run(context.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(isolate, try_catch.Exception());
  CHECK_EQ(0,
           strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
}


THREADED_TEST(DefinePropertyOnDefineGetterSetter) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, nullptr, v8_str("donut"));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  Local<Script> script_desc = v8_compile(
      "var prop ="
      "Object.getOwnPropertyDescriptor( "
      "obj, 'x');"
      "prop.configurable;");
  Local<Value> result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(result->BooleanValue(isolate));

  Local<Script> script_define = v8_compile(
      "var desc = {get: function(){return 42; },"
      "            configurable: true };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run(context.local()).ToLocalChecked();
  CHECK(result->Equals(context.local(), v8_num(42)).FromJust());

  result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(result->BooleanValue(isolate));

  script_define = v8_compile(
      "var desc = {get: function(){return 43; },"
      "            configurable: false };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run(context.local()).ToLocalChecked();
  CHECK(result->Equals(context.local(), v8_num(43)).FromJust());

  result = script_desc->Run(context.local()).ToLocalChecked();
  CHECK(!result->BooleanValue(isolate));

  v8::TryCatch try_catch(isolate);
  CHECK(script_define->Run(context.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(isolate, try_catch.Exception());
  CHECK_EQ(0,
           strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
}


static v8::Local<v8::Object> GetGlobalProperty(LocalContext* context,
                                               char const* name) {
  return v8::Local<v8::Object>::Cast(
      (*context)
          ->Global()
          ->Get(CcTest::isolate()->GetCurrentContext(), v8_str(name))
          .ToLocalChecked());
}


THREADED_TEST(DefineAPIAccessorOnObject) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj1"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("var obj2 = {};");

  CHECK(CompileRun("obj1.x")->IsUndefined());
  CHECK(CompileRun("obj2.x")->IsUndefined());

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"))
            .FromJust());

  ExpectString("obj1.x", "x");
  CHECK(CompileRun("obj2.x")->IsUndefined());

  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"))
            .FromJust());

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CompileRun(
      "Object.defineProperty(obj1, 'x',"
      "{ get: function() { return 'y'; }, configurable: true })");

  ExpectString("obj1.x", "y");
  ExpectString("obj2.x", "x");

  CompileRun(
      "Object.defineProperty(obj2, 'x',"
      "{ get: function() { return 'y'; }, configurable: true })");

  ExpectString("obj1.x", "y");
  ExpectString("obj2.x", "y");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"))
            .FromJust());
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"))
            .FromJust());

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  // Define getters/setters, but now make them not configurable.
  CompileRun(
      "Object.defineProperty(obj1, 'x',"
      "{ get: function() { return 'z'; }, configurable: false })");
  CompileRun(
      "Object.defineProperty(obj2, 'x',"
      "{ get: function() { return 'z'; }, configurable: false })");
  ExpectTrue("!Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("!Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  ExpectString("obj1.x", "z");
  ExpectString("obj2.x", "z");

  CHECK(!GetGlobalProperty(&context, "obj1")
             ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                           v8_str("donut"))
             .FromJust());
  CHECK(!GetGlobalProperty(&context, "obj2")
             ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                           v8_str("donut"))
             .FromJust());

  ExpectString("obj1.x", "z");
  ExpectString("obj2.x", "z");
}


THREADED_TEST(DontDeleteAPIAccessorsCannotBeOverriden) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj1"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("var obj2 = {};");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"), v8::DEFAULT, v8::DontDelete)
            .FromJust());
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("donut"), v8::DEFAULT, v8::DontDelete)
            .FromJust());

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("!Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("!Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CHECK(!GetGlobalProperty(&context, "obj1")
             ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                           v8_str("donut"))
             .FromJust());
  CHECK(!GetGlobalProperty(&context, "obj2")
             ->SetAccessor(context.local(), v8_str("x"), GetXValue, nullptr,
                           v8_str("donut"))
             .FromJust());

  {
    v8::TryCatch try_catch(isolate);
    CompileRun(
        "Object.defineProperty(obj1, 'x',"
        "{get: function() { return 'func'; }})");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value(isolate, try_catch.Exception());
    CHECK_EQ(
        0, strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
  }
  {
    v8::TryCatch try_catch(isolate);
    CompileRun(
        "Object.defineProperty(obj2, 'x',"
        "{get: function() { return 'func'; }})");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value(isolate, try_catch.Exception());
    CHECK_EQ(
        0, strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
  }
}


static void Get239Value(Local<Name> name,
                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CHECK(info.Data()
            ->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("donut"))
            .FromJust());
  CHECK(name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("239"))
            .FromJust());
  info.GetReturnValue().Set(name);
}


THREADED_TEST(ElementAPIAccessor) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj1"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("var obj2 = {};");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(context.local(), v8_str("239"), Get239Value, nullptr,
                          v8_str("donut"))
            .FromJust());
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(context.local(), v8_str("239"), Get239Value, nullptr,
                          v8_str("donut"))
            .FromJust());

  ExpectString("obj1[239]", "239");
  ExpectString("obj2[239]", "239");
  ExpectString("obj1['239']", "239");
  ExpectString("obj2['239']", "239");
}


v8::Persistent<Value> xValue;


static void SetXValue(Local<Name> name, Local<Value> value,
                      const v8::PropertyCallbackInfo<void>& info) {
  Local<Context> context = info.GetIsolate()->GetCurrentContext();
  CHECK(value->Equals(context, v8_num(4)).FromJust());
  CHECK(info.Data()->Equals(context, v8_str("donut")).FromJust());
  CHECK(name->Equals(context, v8_str("x")).FromJust());
  CHECK(xValue.IsEmpty());
  xValue.Reset(info.GetIsolate(), value);
}


THREADED_TEST(SimplePropertyWrite) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, SetXValue, v8_str("donut"));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  Local<Script> script = v8_compile("obj.x = 4");
  for (int i = 0; i < 10; i++) {
    CHECK(xValue.IsEmpty());
    script->Run(context.local()).ToLocalChecked();
    CHECK(v8_num(4)
              ->Equals(context.local(),
                       Local<Value>::New(CcTest::isolate(), xValue))
              .FromJust());
    xValue.Reset();
  }
}


THREADED_TEST(SetterOnly) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), nullptr, SetXValue, v8_str("donut"));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  Local<Script> script = v8_compile("obj.x = 4; obj.x");
  for (int i = 0; i < 10; i++) {
    CHECK(xValue.IsEmpty());
    script->Run(context.local()).ToLocalChecked();
    CHECK(v8_num(4)
              ->Equals(context.local(),
                       Local<Value>::New(CcTest::isolate(), xValue))
              .FromJust());
    xValue.Reset();
  }
}


THREADED_TEST(NoAccessors) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"),
                     static_cast<v8::AccessorGetterCallback>(nullptr), nullptr,
                     v8_str("donut"));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  Local<Script> script = v8_compile("obj.x = 4; obj.x");
  for (int i = 0; i < 10; i++) {
    script->Run(context.local()).ToLocalChecked();
  }
}


THREADED_TEST(MultiContexts) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("dummy"),
             v8::FunctionTemplate::New(isolate, DummyCallHandler));

  Local<String> password = v8_str("Password");

  // Create an environment
  LocalContext context0(nullptr, templ);
  context0->SetSecurityToken(password);
  v8::Local<v8::Object> global0 = context0->Global();
  CHECK(global0->Set(context0.local(), v8_str("custom"), v8_num(1234))
            .FromJust());
  CHECK_EQ(1234, global0->Get(context0.local(), v8_str("custom"))
                     .ToLocalChecked()
                     ->Int32Value(context0.local())
                     .FromJust());

  // Create an independent environment
  LocalContext context1(nullptr, templ);
  context1->SetSecurityToken(password);
  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1.local(), v8_str("custom"), v8_num(1234))
            .FromJust());
  CHECK(!global0->Equals(context1.local(), global1).FromJust());
  CHECK_EQ(1234, global0->Get(context1.local(), v8_str("custom"))
                     .ToLocalChecked()
                     ->Int32Value(context0.local())
                     .FromJust());
  CHECK_EQ(1234, global1->Get(context1.local(), v8_str("custom"))
                     .ToLocalChecked()
                     ->Int32Value(context1.local())
                     .FromJust());

  // Now create a new context with the old global
  LocalContext context2(nullptr, templ, global1);
  context2->SetSecurityToken(password);
  v8::Local<v8::Object> global2 = context2->Global();
  CHECK(global1->Equals(context2.local(), global2).FromJust());
  CHECK_EQ(0, global1->Get(context2.local(), v8_str("custom"))
                  .ToLocalChecked()
                  ->Int32Value(context1.local())
                  .FromJust());
  CHECK_EQ(0, global2->Get(context2.local(), v8_str("custom"))
                  .ToLocalChecked()
                  ->Int32Value(context2.local())
                  .FromJust());
}


THREADED_TEST(FunctionPrototypeAcrossContexts) {
  // Make sure that functions created by cloning boilerplates cannot
  // communicate through their __proto__ field.

  v8::HandleScope scope(CcTest::isolate());

  LocalContext env0;
  v8::Local<v8::Object> global0 = env0->Global();
  v8::Local<v8::Object> object0 = global0->Get(env0.local(), v8_str("Object"))
                                      .ToLocalChecked()
                                      .As<v8::Object>();
  v8::Local<v8::Object> tostring0 =
      object0->Get(env0.local(), v8_str("toString"))
          .ToLocalChecked()
          .As<v8::Object>();
  v8::Local<v8::Object> proto0 =
      tostring0->Get(env0.local(), v8_str("__proto__"))
          .ToLocalChecked()
          .As<v8::Object>();
  CHECK(proto0->Set(env0.local(), v8_str("custom"), v8_num(1234)).FromJust());

  LocalContext env1;
  v8::Local<v8::Object> global1 = env1->Global();
  v8::Local<v8::Object> object1 = global1->Get(env1.local(), v8_str("Object"))
                                      .ToLocalChecked()
                                      .As<v8::Object>();
  v8::Local<v8::Object> tostring1 =
      object1->Get(env1.local(), v8_str("toString"))
          .ToLocalChecked()
          .As<v8::Object>();
  v8::Local<v8::Object> proto1 =
      tostring1->Get(env1.local(), v8_str("__proto__"))
          .ToLocalChecked()
          .As<v8::Object>();
  CHECK(!proto1->Has(env1.local(), v8_str("custom")).FromJust());
}


THREADED_TEST(Regress892105) {
  // Make sure that object and array literals created by cloning
  // boilerplates cannot communicate through their __proto__
  // field. This is rather difficult to check, but we try to add stuff
  // to Object.prototype and Array.prototype and create a new
  // environment. This should succeed.

  v8::HandleScope scope(CcTest::isolate());

  Local<String> source = v8_str(
      "Object.prototype.obj = 1234;"
      "Array.prototype.arr = 4567;"
      "8901");

  LocalContext env0;
  Local<Script> script0 = v8_compile(source);
  CHECK_EQ(8901.0, script0->Run(env0.local())
                       .ToLocalChecked()
                       ->NumberValue(env0.local())
                       .FromJust());

  LocalContext env1;
  Local<Script> script1 = v8_compile(source);
  CHECK_EQ(8901.0, script1->Run(env1.local())
                       .ToLocalChecked()
                       ->NumberValue(env1.local())
                       .FromJust());
}

static void ReturnThis(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(args.This());
}

THREADED_TEST(UndetectableObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<v8::FunctionTemplate> desc =
      v8::FunctionTemplate::New(env->GetIsolate());
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable
  desc->InstanceTemplate()->SetCallAsFunctionHandler(ReturnThis);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(
      env->Global()->Set(env.local(), v8_str("undetectable"), obj).FromJust());

  ExpectString("undetectable.toString()", "[object Object]");
  ExpectString("typeof undetectable", "undefined");
  ExpectString("typeof(undetectable)", "undefined");
  ExpectBoolean("typeof undetectable == 'undefined'", true);
  ExpectBoolean("typeof undetectable == 'object'", false);
  ExpectBoolean("if (undetectable) { true; } else { false; }", false);
  ExpectBoolean("!undetectable", true);

  ExpectObject("true&&undetectable", obj);
  ExpectBoolean("false&&undetectable", false);
  ExpectBoolean("true||undetectable", true);
  ExpectObject("false||undetectable", obj);

  ExpectObject("undetectable&&true", obj);
  ExpectObject("undetectable&&false", obj);
  ExpectBoolean("undetectable||true", true);
  ExpectBoolean("undetectable||false", false);

  ExpectBoolean("undetectable==null", true);
  ExpectBoolean("null==undetectable", true);
  ExpectBoolean("undetectable==undefined", true);
  ExpectBoolean("undefined==undetectable", true);
  ExpectBoolean("undetectable==undetectable", true);


  ExpectBoolean("undetectable===null", false);
  ExpectBoolean("null===undetectable", false);
  ExpectBoolean("undetectable===undefined", false);
  ExpectBoolean("undefined===undetectable", false);
  ExpectBoolean("undetectable===undetectable", true);
}


THREADED_TEST(VoidLiteral) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable
  desc->InstanceTemplate()->SetCallAsFunctionHandler(ReturnThis);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(
      env->Global()->Set(env.local(), v8_str("undetectable"), obj).FromJust());

  ExpectBoolean("undefined == void 0", true);
  ExpectBoolean("undetectable == void 0", true);
  ExpectBoolean("null == void 0", true);
  ExpectBoolean("undefined === void 0", true);
  ExpectBoolean("undetectable === void 0", false);
  ExpectBoolean("null === void 0", false);

  ExpectBoolean("void 0 == undefined", true);
  ExpectBoolean("void 0 == undetectable", true);
  ExpectBoolean("void 0 == null", true);
  ExpectBoolean("void 0 === undefined", true);
  ExpectBoolean("void 0 === undetectable", false);
  ExpectBoolean("void 0 === null", false);

  ExpectString(
      "(function() {"
      "  try {"
      "    return x === void 0;"
      "  } catch(e) {"
      "    return e.toString();"
      "  }"
      "})()",
      "ReferenceError: x is not defined");
  ExpectString(
      "(function() {"
      "  try {"
      "    return void 0 === x;"
      "  } catch(e) {"
      "    return e.toString();"
      "  }"
      "})()",
      "ReferenceError: x is not defined");
}


THREADED_TEST(ExtensibleOnUndetectable) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable
  desc->InstanceTemplate()->SetCallAsFunctionHandler(ReturnThis);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(
      env->Global()->Set(env.local(), v8_str("undetectable"), obj).FromJust());

  Local<String> source = v8_str(
      "undetectable.x = 42;"
      "undetectable.x");

  Local<Script> script = v8_compile(source);

  CHECK(v8::Integer::New(isolate, 42)
            ->Equals(env.local(), script->Run(env.local()).ToLocalChecked())
            .FromJust());

  ExpectBoolean("Object.isExtensible(undetectable)", true);

  source = v8_str("Object.preventExtensions(undetectable);");
  script = v8_compile(source);
  script->Run(env.local()).ToLocalChecked();
  ExpectBoolean("Object.isExtensible(undetectable)", false);

  source = v8_str("undetectable.y = 2000;");
  script = v8_compile(source);
  script->Run(env.local()).ToLocalChecked();
  ExpectBoolean("undetectable.y == undefined", true);
}

THREADED_TEST(ConstructCallWithUndetectable) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable
  desc->InstanceTemplate()->SetCallAsFunctionHandler(ReturnThis);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(
      env->Global()->Set(env.local(), v8_str("undetectable"), obj).FromJust());

  // Undetectable object cannot be called as constructor.
  v8::TryCatch try_catch(env->GetIsolate());
  CHECK(CompileRun("new undetectable()").IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(env->GetIsolate(), try_catch.Exception());
  CHECK_EQ(0, strcmp("TypeError: undetectable is not a constructor",
                     *exception_value));
}

static int increment_callback_counter = 0;

static void IncrementCounterConstructCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  increment_callback_counter++;
  CHECK(Local<Object>::Cast(args.NewTarget())
            ->Set(args.GetIsolate()->GetCurrentContext(), v8_str("counter"),
                  v8_num(increment_callback_counter))
            .FromJust());
  args.GetReturnValue().Set(args.NewTarget());
}

THREADED_TEST(SetCallAsFunctionHandlerConstructor) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->SetCallAsFunctionHandler(
      IncrementCounterConstructCallback);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("Counter"), obj).FromJust());

  ExpectInt32("(new Counter()).counter", 1);
  CHECK_EQ(1, increment_callback_counter);
  ExpectInt32("(new Counter()).counter", 2);
  CHECK_EQ(2, increment_callback_counter);
}
// The point of this test is type checking. We run it only so compilers
// don't complain about an unused function.
TEST(PersistentHandles) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<String> str = v8_str("foo");
  v8::Persistent<String> p_str(isolate, str);
  p_str.Reset();
  Local<Script> scr = v8_compile("");
  v8::Persistent<Script> p_scr(isolate, scr);
  p_scr.Reset();
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  v8::Persistent<ObjectTemplate> p_templ(isolate, templ);
  p_templ.Reset();
}


static void HandleLogDelegator(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
}


THREADED_TEST(GlobalObjectTemplate) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  Local<ObjectTemplate> global_template = ObjectTemplate::New(isolate);
  global_template->Set(v8_str("JSNI_Log"),
                       v8::FunctionTemplate::New(isolate, HandleLogDelegator));
  v8::Local<Context> context = Context::New(isolate, nullptr, global_template);
  Context::Scope context_scope(context);
  CompileRun("JSNI_Log('LOG')");
}


static const char* kSimpleExtensionSource =
    "function Foo() {"
    "  return 4;"
    "}";


TEST(SimpleExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("simpletest", kSimpleExtensionSource));
  const char* extension_names[] = {"simpletest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("Foo()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 4))
            .FromJust());
}


static const char* kStackTraceFromExtensionSource =
    "function foo() {"
    "  throw new Error();"
    "}"
    "function bar() {"
    "  foo();"
    "}";


TEST(StackTraceInExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>(
      "stacktracetest", kStackTraceFromExtensionSource));
  const char* extension_names[] = {"stacktracetest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  CompileRun(
      "function user() { bar(); }"
      "var error;"
      "try{ user(); } catch (e) { error = e; }");
  CHECK_EQ(-1, v8_run_int32value(v8_compile("error.stack.indexOf('foo')")));
  CHECK_EQ(-1, v8_run_int32value(v8_compile("error.stack.indexOf('bar')")));
  CHECK_NE(-1, v8_run_int32value(v8_compile("error.stack.indexOf('user')")));
}


TEST(NullExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>("nulltest", nullptr));
  const char* extension_names[] = {"nulltest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("1+3");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 4))
            .FromJust());
}

static const char* kEmbeddedExtensionSource =
    "function Ret54321(){return 54321;}~~@@$"
    "$%% THIS IS A SERIES OF NON-nullptr-TERMINATED STRINGS.";
static const int kEmbeddedExtensionSourceValidLen = 34;


TEST(ExtensionMissingSourceLength) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>(
      "srclentest_fail", kEmbeddedExtensionSource));
  const char* extension_names[] = {"srclentest_fail"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK_NULL(*context);
}


TEST(ExtensionWithSourceLength) {
  for (int source_len = kEmbeddedExtensionSourceValidLen - 1;
       source_len <= kEmbeddedExtensionSourceValidLen + 1; ++source_len) {
    v8::HandleScope handle_scope(CcTest::isolate());
    i::ScopedVector<char> extension_name(32);
    i::SNPrintF(extension_name, "ext #%d", source_len);
    v8::RegisterExtension(v8::base::make_unique<Extension>(
        extension_name.begin(), kEmbeddedExtensionSource, 0, nullptr,
        source_len));
    const char* extension_names[1] = {extension_name.begin()};
    v8::ExtensionConfiguration extensions(1, extension_names);
    v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
    if (source_len == kEmbeddedExtensionSourceValidLen) {
      Context::Scope lock(context);
      v8::Local<Value> result = CompileRun("Ret54321()");
      CHECK(v8::Integer::New(CcTest::isolate(), 54321)
                ->Equals(context, result)
                .FromJust());
    } else {
      // Anything but exactly the right length should fail to compile.
      CHECK_NULL(*context);
    }
  }
}


static const char* kEvalExtensionSource1 =
    "function UseEval1() {"
    "  var x = 42;"
    "  return eval('x');"
    "}";


static const char* kEvalExtensionSource2 =
    "(function() {"
    "  var x = 42;"
    "  function e() {"
    "    return eval('x');"
    "  }"
    "  this.UseEval2 = e;"
    "})()";


TEST(UseEvalFromExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("evaltest1", kEvalExtensionSource1));
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("evaltest2", kEvalExtensionSource2));
  const char* extension_names[] = {"evaltest1", "evaltest2"};
  v8::ExtensionConfiguration extensions(2, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("UseEval1()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 42))
            .FromJust());
  result = CompileRun("UseEval2()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 42))
            .FromJust());
}


static const char* kWithExtensionSource1 =
    "function UseWith1() {"
    "  var x = 42;"
    "  with({x:87}) { return x; }"
    "}";


static const char* kWithExtensionSource2 =
    "(function() {"
    "  var x = 42;"
    "  function e() {"
    "    with ({x:87}) { return x; }"
    "  }"
    "  this.UseWith2 = e;"
    "})()";


TEST(UseWithFromExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("withtest1", kWithExtensionSource1));
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("withtest2", kWithExtensionSource2));
  const char* extension_names[] = {"withtest1", "withtest2"};
  v8::ExtensionConfiguration extensions(2, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("UseWith1()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 87))
            .FromJust());
  result = CompileRun("UseWith2()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 87))
            .FromJust());
}


TEST(AutoExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  auto extension =
      v8::base::make_unique<Extension>("autotest", kSimpleExtensionSource);
  extension->set_auto_enable(true);
  v8::RegisterExtension(std::move(extension));
  v8::Local<Context> context = Context::New(CcTest::isolate());
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("Foo()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 4))
            .FromJust());
}


static const char* kSyntaxErrorInExtensionSource = "[";


// Test that a syntax error in an extension does not cause a fatal
// error but results in an empty context.
TEST(SyntaxErrorExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>(
      "syntaxerror", kSyntaxErrorInExtensionSource));
  const char* extension_names[] = {"syntaxerror"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


static const char* kExceptionInExtensionSource = "throw 42";


// Test that an exception when installing an extension does not cause
// a fatal error but results in an empty context.
TEST(ExceptionExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>(
      "exception", kExceptionInExtensionSource));
  const char* extension_names[] = {"exception"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}

static const char* kNativeCallInExtensionSource =
    "function call_runtime_last_index_of(x) {"
    "  return %StringLastIndexOf(x, 'bob');"
    "}";

static const char* kNativeCallTest =
    "call_runtime_last_index_of('bobbobboellebobboellebobbob');";

// Test that a native runtime calls are supported in extensions.
TEST(NativeCallInExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(v8::base::make_unique<Extension>(
      "nativecall", kNativeCallInExtensionSource));
  const char* extension_names[] = {"nativecall"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun(kNativeCallTest);
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 24))
            .FromJust());
}


class NativeFunctionExtension : public Extension {
 public:
  NativeFunctionExtension(const char* name, const char* source,
                          v8::FunctionCallback fun = &Echo)
      : Extension(name, source), function_(fun) {}

  v8::Local<v8::FunctionTemplate> GetNativeFunctionTemplate(
      v8::Isolate* isolate, v8::Local<v8::String> name) override {
    return v8::FunctionTemplate::New(isolate, function_);
  }

  static void Echo(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() >= 1) args.GetReturnValue().Set(args[0]);
  }

 private:
  v8::FunctionCallback function_;
};


TEST(NativeFunctionDeclaration) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedecl";
  v8::RegisterExtension(v8::base::make_unique<NativeFunctionExtension>(
      name, "native function foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Local<Value> result = CompileRun("foo(42);");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 42))
            .FromJust());
}


TEST(NativeFunctionDeclarationError) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedeclerr";
  // Syntax error in extension code.
  v8::RegisterExtension(v8::base::make_unique<NativeFunctionExtension>(
      name, "native\nfunction foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


TEST(NativeFunctionDeclarationErrorEscape) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedeclerresc";
  // Syntax error in extension code - escape code in "native" means that
  // it's not treated as a keyword.
  v8::RegisterExtension(v8::base::make_unique<NativeFunctionExtension>(
      name, "nativ\\u0065 function foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


static void CheckDependencies(const char* name, const char* expected) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::ExtensionConfiguration config(1, &name);
  LocalContext context(&config);
  CHECK(
      v8_str(expected)
          ->Equals(context.local(), context->Global()
                                        ->Get(context.local(), v8_str("loaded"))
                                        .ToLocalChecked())
          .FromJust());
}


/*
 * Configuration:
 *
 *     /-- B <--\
 * A <-          -- D <-- E
 *     \-- C <--/
 */
THREADED_TEST(ExtensionDependency) {
  static const char* kEDeps[] = {"D"};
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("E", "this.loaded += 'E';", 1, kEDeps));
  static const char* kDDeps[] = {"B", "C"};
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("D", "this.loaded += 'D';", 2, kDDeps));
  static const char* kBCDeps[] = {"A"};
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("B", "this.loaded += 'B';", 1, kBCDeps));
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("C", "this.loaded += 'C';", 1, kBCDeps));
  v8::RegisterExtension(
      v8::base::make_unique<Extension>("A", "this.loaded += 'A';"));
  CheckDependencies("A", "undefinedA");
  CheckDependencies("B", "undefinedAB");
  CheckDependencies("C", "undefinedAC");
  CheckDependencies("D", "undefinedABCD");
  CheckDependencies("E", "undefinedABCDE");
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[2] = {"C", "E"};
  v8::ExtensionConfiguration config(2, exts);
  LocalContext context(&config);
  CHECK(
      v8_str("undefinedACBDE")
          ->Equals(context.local(), context->Global()
                                        ->Get(context.local(), v8_str("loaded"))
                                        .ToLocalChecked())
          .FromJust());
}


static const char* kExtensionTestScript =
    "native function A();"
    "native function B();"
    "native function C();"
    "function Foo(i) {"
    "  if (i == 0) return A();"
    "  if (i == 1) return B();"
    "  if (i == 2) return C();"
    "}";


static void CallFun(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  if (args.IsConstructCall()) {
    CHECK(args.This()
              ->Set(args.GetIsolate()->GetCurrentContext(), v8_str("data"),
                    args.Data())
              .FromJust());
    args.GetReturnValue().SetNull();
    return;
  }
  args.GetReturnValue().Set(args.Data());
}


class FunctionExtension : public Extension {
 public:
  FunctionExtension() : Extension("functiontest", kExtensionTestScript) {}
  v8::Local<v8::FunctionTemplate> GetNativeFunctionTemplate(
      v8::Isolate* isolate, v8::Local<String> name) override;
};


static int lookup_count = 0;
v8::Local<v8::FunctionTemplate> FunctionExtension::GetNativeFunctionTemplate(
    v8::Isolate* isolate, v8::Local<String> name) {
  lookup_count++;
  if (name->StrictEquals(v8_str("A"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 8));
  } else if (name->StrictEquals(v8_str("B"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 7));
  } else if (name->StrictEquals(v8_str("C"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 6));
  } else {
    return v8::Local<v8::FunctionTemplate>();
  }
}


THREADED_TEST(FunctionLookup) {
  v8::RegisterExtension(v8::base::make_unique<FunctionExtension>());
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[1] = {"functiontest"};
  v8::ExtensionConfiguration config(1, exts);
  LocalContext context(&config);
  CHECK_EQ(3, lookup_count);
  CHECK(v8::Integer::New(CcTest::isolate(), 8)
            ->Equals(context.local(), CompileRun("Foo(0)"))
            .FromJust());
  CHECK(v8::Integer::New(CcTest::isolate(), 7)
            ->Equals(context.local(), CompileRun("Foo(1)"))
            .FromJust());
  CHECK(v8::Integer::New(CcTest::isolate(), 6)
            ->Equals(context.local(), CompileRun("Foo(2)"))
            .FromJust());
}


THREADED_TEST(NativeFunctionConstructCall) {
  v8::RegisterExtension(v8::base::make_unique<FunctionExtension>());
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[1] = {"functiontest"};
  v8::ExtensionConfiguration config(1, exts);
  LocalContext context(&config);
  for (int i = 0; i < 10; i++) {
    // Run a few times to ensure that allocation of objects doesn't
    // change behavior of a constructor function.
    CHECK(v8::Integer::New(CcTest::isolate(), 8)
              ->Equals(context.local(), CompileRun("(new A()).data"))
              .FromJust());
    CHECK(v8::Integer::New(CcTest::isolate(), 7)
              ->Equals(context.local(), CompileRun("(new B()).data"))
              .FromJust());
    CHECK(v8::Integer::New(CcTest::isolate(), 6)
              ->Equals(context.local(), CompileRun("(new C()).data"))
              .FromJust());
  }
}


static const char* last_location;
static const char* last_message;
void StoringErrorCallback(const char* location, const char* message) {
  if (last_location == nullptr) {
    last_location = location;
    last_message = message;
  }
}


// ErrorReporting creates a circular extensions configuration and
// tests that the fatal error handler gets called.  This renders V8
// unusable and therefore this test cannot be run in parallel.
TEST(ErrorReporting) {
  CcTest::isolate()->SetFatalErrorHandler(StoringErrorCallback);
  static const char* aDeps[] = {"B"};
  v8::RegisterExtension(v8::base::make_unique<Extension>("A", "", 1, aDeps));
  static const char* bDeps[] = {"A"};
  v8::RegisterExtension(v8::base::make_unique<Extension>("B", "", 1, bDeps));
  last_location = nullptr;
  v8::ExtensionConfiguration config(1, bDeps);
  v8::Local<Context> context = Context::New(CcTest::isolate(), &config);
  CHECK(context.IsEmpty());
  CHECK(last_location);
}

static size_t dcheck_count;
void DcheckErrorCallback(const char* file, int line, const char* message) {
  last_message = message;
  ++dcheck_count;
}

TEST(DcheckErrorHandler) {
  V8::SetDcheckErrorHandler(DcheckErrorCallback);

  last_message = nullptr;
  dcheck_count = 0;

  DCHECK(false && "w00t");
#ifdef DEBUG
  CHECK_EQ(dcheck_count, 1);
  CHECK(last_message);
  CHECK(std::string(last_message).find("w00t") != std::string::npos);
#else
  // The DCHECK should be a noop in non-DEBUG builds.
  CHECK_EQ(dcheck_count, 0);
#endif
}

static void MissingScriptInfoMessageListener(v8::Local<v8::Message> message,
                                             v8::Local<Value> data) {
  v8::Isolate* isolate = CcTest::isolate();
  Local<Context> context = isolate->GetCurrentContext();
  CHECK(message->GetScriptOrigin().ResourceName()->IsUndefined());
  CHECK(v8::Undefined(isolate)
            ->Equals(context, message->GetScriptOrigin().ResourceName())
            .FromJust());
  message->GetLineNumber(context).FromJust();
  message->GetSourceLine(context).ToLocalChecked();
}


THREADED_TEST(ErrorWithMissingScriptInfo) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  context->GetIsolate()->AddMessageListener(MissingScriptInfoMessageListener);
  CompileRun("throw Error()");
  context->GetIsolate()->RemoveMessageListeners(
      MissingScriptInfoMessageListener);
}


struct FlagAndPersistent {
  bool flag;
  v8::Global<v8::Object> handle;
};

static void SetFlag(const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->flag = true;
  data.GetParameter()->handle.Reset();
}

static void IndependentWeakHandle(bool global_gc, bool interlinked) {
  i::FLAG_stress_incremental_marking = false;
  // Parallel scavenge introduces too much fragmentation.
  i::FLAG_parallel_scavenge = false;
  v8::Isolate* iso = CcTest::isolate();
  v8::HandleScope scope(iso);
  v8::Local<Context> context = Context::New(iso);
  Context::Scope context_scope(context);

  FlagAndPersistent object_a, object_b;

  size_t big_heap_size = 0;
  size_t big_array_size = 0;

  {
    v8::HandleScope handle_scope(iso);
    Local<Object> a(v8::Object::New(iso));
    Local<Object> b(v8::Object::New(iso));
    object_a.handle.Reset(iso, a);
    object_b.handle.Reset(iso, b);
    if (interlinked) {
      a->Set(context, v8_str("x"), b).FromJust();
      b->Set(context, v8_str("x"), a).FromJust();
    }
    if (global_gc) {
      CcTest::CollectAllGarbage();
    } else {
      CcTest::CollectGarbage(i::NEW_SPACE);
    }
    v8::Local<Value> big_array = v8::Array::New(CcTest::isolate(), 5000);
    // Verify that we created an array where the space was reserved up front.
    big_array_size =
        v8::internal::JSArray::cast(*v8::Utils::OpenHandle(*big_array))
            .elements()
            .Size();
    CHECK_LE(20000, big_array_size);
    a->Set(context, v8_str("y"), big_array).FromJust();
    big_heap_size = CcTest::heap()->SizeOfObjects();
  }

  object_a.flag = false;
  object_b.flag = false;
  object_a.handle.SetWeak(&object_a, &SetFlag,
                          v8::WeakCallbackType::kParameter);
  object_b.handle.SetWeak(&object_b, &SetFlag,
                          v8::WeakCallbackType::kParameter);
  if (global_gc) {
    CcTest::CollectAllGarbage();
  } else {
    CcTest::CollectGarbage(i::NEW_SPACE);
  }
  // A single GC should be enough to reclaim the memory, since we are using
  // phantom handles.
  CHECK_GT(big_heap_size - big_array_size, CcTest::heap()->SizeOfObjects());
  CHECK(object_a.flag);
  CHECK(object_b.flag);
}

TEST(IndependentWeakHandle) {
  IndependentWeakHandle(false, false);
  IndependentWeakHandle(false, true);
  IndependentWeakHandle(true, false);
  IndependentWeakHandle(true, true);
}

class Trivial {
 public:
  explicit Trivial(int x) : x_(x) {}

  int x() { return x_; }
  void set_x(int x) { x_ = x; }

 private:
  int x_;
};


class Trivial2 {
 public:
  Trivial2(int x, int y) : y_(y), x_(x) {}

  int x() { return x_; }
  void set_x(int x) { x_ = x; }

  int y() { return y_; }
  void set_y(int y) { y_ = y; }

 private:
  int y_;
  int x_;
};

void CheckInternalFields(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  v8::Persistent<v8::Object>* handle = data.GetParameter();
  handle->Reset();
  Trivial* t1 = reinterpret_cast<Trivial*>(data.GetInternalField(0));
  Trivial2* t2 = reinterpret_cast<Trivial2*>(data.GetInternalField(1));
  CHECK_EQ(42, t1->x());
  CHECK_EQ(103, t2->x());
  t1->set_x(1729);
  t2->set_x(33550336);
}

void InternalFieldCallback(bool global_gc) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  Trivial* t1;
  Trivial2* t2;
  instance_templ->SetInternalFieldCount(2);
  v8::Persistent<v8::Object> handle;
  {
    v8::HandleScope scope(isolate);
    Local<v8::Object> obj = templ->GetFunction(env.local())
                                .ToLocalChecked()
                                ->NewInstance(env.local())
                                .ToLocalChecked();
    handle.Reset(isolate, obj);
    CHECK_EQ(2, obj->InternalFieldCount());
    CHECK(obj->GetInternalField(0)->IsUndefined());
    t1 = new Trivial(42);
    t2 = new Trivial2(103, 9);

    obj->SetAlignedPointerInInternalField(0, t1);
    t1 = reinterpret_cast<Trivial*>(obj->GetAlignedPointerFromInternalField(0));
    CHECK_EQ(42, t1->x());

    obj->SetAlignedPointerInInternalField(1, t2);
    t2 =
        reinterpret_cast<Trivial2*>(obj->GetAlignedPointerFromInternalField(1));
    CHECK_EQ(103, t2->x());

    handle.SetWeak<v8::Persistent<v8::Object>>(
        &handle, CheckInternalFields, v8::WeakCallbackType::kInternalFields);
  }
  if (global_gc) {
    CcTest::CollectAllGarbage();
  } else {
    CcTest::CollectGarbage(i::NEW_SPACE);
  }

  CHECK_EQ(1729, t1->x());
  CHECK_EQ(33550336, t2->x());

  delete t1;
  delete t2;
}

THREADED_TEST(InternalFieldCallback) {
  InternalFieldCallback(false);
  InternalFieldCallback(true);
}

static void ResetUseValueAndSetFlag(
    const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  // Blink will reset the handle, and then use the other handle, so they
  // can't use the same backing slot.
  data.GetParameter()->handle.Reset();
  data.GetParameter()->flag = true;
}

void v8::internal::heap::HeapTester::ResetWeakHandle(bool global_gc) {
  using v8::Context;
  using v8::Local;
  using v8::Object;

  v8::Isolate* iso = CcTest::isolate();
  v8::HandleScope scope(iso);
  v8::Local<Context> context = Context::New(iso);
  Context::Scope context_scope(context);

  FlagAndPersistent object_a, object_b;

  {
    v8::HandleScope handle_scope(iso);
    Local<Object> a(v8::Object::New(iso));
    Local<Object> b(v8::Object::New(iso));
    object_a.handle.Reset(iso, a);
    object_b.handle.Reset(iso, b);
    if (global_gc) {
      CcTest::PreciseCollectAllGarbage();
    } else {
      CcTest::CollectGarbage(i::NEW_SPACE);
    }
  }

  object_a.flag = false;
  object_b.flag = false;
  object_a.handle.SetWeak(&object_a, &ResetUseValueAndSetFlag,
                          v8::WeakCallbackType::kParameter);
  object_b.handle.SetWeak(&object_b, &ResetUseValueAndSetFlag,
                          v8::WeakCallbackType::kParameter);
  if (global_gc) {
    CcTest::PreciseCollectAllGarbage();
  } else {
    CcTest::CollectGarbage(i::NEW_SPACE);
  }
  CHECK(object_a.flag);
  CHECK(object_b.flag);
}

THREADED_HEAP_TEST(ResetWeakHandle) {
  v8::internal::heap::HeapTester::ResetWeakHandle(false);
  v8::internal::heap::HeapTester::ResetWeakHandle(true);
}

static void InvokeScavenge() { CcTest::CollectGarbage(i::NEW_SPACE); }

static void InvokeMarkSweep() { CcTest::CollectAllGarbage(); }

static void ForceScavenge2(
    const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->flag = true;
  InvokeScavenge();
}

static void ForceScavenge1(
    const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->handle.Reset();
  data.SetSecondPassCallback(ForceScavenge2);
}

static void ForceMarkSweep2(
    const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->flag = true;
  InvokeMarkSweep();
}

static void ForceMarkSweep1(
    const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->handle.Reset();
  data.SetSecondPassCallback(ForceMarkSweep2);
}

THREADED_TEST(GCFromWeakCallbacks) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Locker locker(CcTest::isolate());
  v8::HandleScope scope(isolate);
  v8::Local<Context> context = Context::New(isolate);
  Context::Scope context_scope(context);

  static const int kNumberOfGCTypes = 2;
  using Callback = v8::WeakCallbackInfo<FlagAndPersistent>::Callback;
  Callback gc_forcing_callback[kNumberOfGCTypes] = {&ForceScavenge1,
                                                    &ForceMarkSweep1};

  using GCInvoker = void (*)();
  GCInvoker invoke_gc[kNumberOfGCTypes] = {&InvokeScavenge, &InvokeMarkSweep};

  for (int outer_gc = 0; outer_gc < kNumberOfGCTypes; outer_gc++) {
    for (int inner_gc = 0; inner_gc < kNumberOfGCTypes; inner_gc++) {
      FlagAndPersistent object;
      {
        v8::HandleScope handle_scope(isolate);
        object.handle.Reset(isolate, v8::Object::New(isolate));
      }
      object.flag = false;
      object.handle.SetWeak(&object, gc_forcing_callback[inner_gc],
                            v8::WeakCallbackType::kParameter);
      invoke_gc[outer_gc]();
      EmptyMessageQueues(isolate);
      CHECK(object.flag);
    }
  }
}

v8::Local<Function> args_fun;


static void ArgumentsTestCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();
  CHECK_EQ(3, args.Length());
  CHECK(v8::Integer::New(isolate, 1)->Equals(context, args[0]).FromJust());
  CHECK(v8::Integer::New(isolate, 2)->Equals(context, args[1]).FromJust());
  CHECK(v8::Integer::New(isolate, 3)->Equals(context, args[2]).FromJust());
  CHECK(v8::Undefined(isolate)->Equals(context, args[3]).FromJust());
  v8::HandleScope scope(args.GetIsolate());
  CcTest::CollectAllGarbage();
}


THREADED_TEST(Arguments) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("f"),
              v8::FunctionTemplate::New(isolate, ArgumentsTestCallback));
  LocalContext context(nullptr, global);
  args_fun = context->Global()
                 ->Get(context.local(), v8_str("f"))
                 .ToLocalChecked()
                 .As<Function>();
  v8_compile("f(1, 2, 3)")->Run(context.local()).ToLocalChecked();
}


static int p_getter_count;
static int p_getter_count2;


static void PGetter(Local<Name> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  p_getter_count++;
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Local<v8::Object> global = context->Global();
  CHECK(
      info.Holder()
          ->Equals(context, global->Get(context, v8_str("o1")).ToLocalChecked())
          .FromJust());
  if (name->Equals(context, v8_str("p1")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o1")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p2")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o2")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p3")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o3")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p4")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o4")).ToLocalChecked())
              .FromJust());
  }
}


static void RunHolderTest(v8::Local<v8::ObjectTemplate> obj) {
  ApiTestFuzzer::Fuzz();
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o1"),
                  obj->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun(
    "o1.__proto__ = { };"
    "var o2 = { __proto__: o1 };"
    "var o3 = { __proto__: o2 };"
    "var o4 = { __proto__: o3 };"
    "for (var i = 0; i < 10; i++) o4.p4;"
    "for (var i = 0; i < 10; i++) o3.p3;"
    "for (var i = 0; i < 10; i++) o2.p2;"
    "for (var i = 0; i < 10; i++) o1.p1;");
}


static void PGetter2(Local<Name> name,
                     const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  p_getter_count2++;
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Local<v8::Object> global = context->Global();
  CHECK(
      info.Holder()
          ->Equals(context, global->Get(context, v8_str("o1")).ToLocalChecked())
          .FromJust());
  if (name->Equals(context, v8_str("p1")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o1")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p2")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o2")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p3")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o3")).ToLocalChecked())
              .FromJust());
  } else if (name->Equals(context, v8_str("p4")).FromJust()) {
    CHECK(info.This()
              ->Equals(context,
                       global->Get(context, v8_str("o4")).ToLocalChecked())
              .FromJust());
  }
}


THREADED_TEST(GetterHolders) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), PGetter);
  obj->SetAccessor(v8_str("p2"), PGetter);
  obj->SetAccessor(v8_str("p3"), PGetter);
  obj->SetAccessor(v8_str("p4"), PGetter);
  p_getter_count = 0;
  RunHolderTest(obj);
  CHECK_EQ(40, p_getter_count);
}


THREADED_TEST(PreInterceptorHolders) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetHandler(v8::NamedPropertyHandlerConfiguration(PGetter2));
  p_getter_count2 = 0;
  RunHolderTest(obj);
  CHECK_EQ(40, p_getter_count2);
}


THREADED_TEST(ObjectInstantiation) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("t"), PGetter2);
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  for (int i = 0; i < 100; i++) {
    v8::HandleScope inner_scope(CcTest::isolate());
    v8::Local<v8::Object> obj =
        templ->NewInstance(context.local()).ToLocalChecked();
    CHECK(!obj->Equals(context.local(), context->Global()
                                            ->Get(context.local(), v8_str("o"))
                                            .ToLocalChecked())
               .FromJust());
    CHECK(
        context->Global()->Set(context.local(), v8_str("o2"), obj).FromJust());
    v8::Local<Value> value = CompileRun("o.__proto__ === o2.__proto__");
    CHECK(v8::True(isolate)->Equals(context.local(), value).FromJust());
    CHECK(context->Global()->Set(context.local(), v8_str("o"), obj).FromJust());
  }
}


static int StrCmp16(uint16_t* a, uint16_t* b) {
  while (true) {
    if (*a == 0 && *b == 0) return 0;
    if (*a != *b) return 0 + *a - *b;
    a++;
    b++;
  }
}


static int StrNCmp16(uint16_t* a, uint16_t* b, int n) {
  while (true) {
    if (n-- == 0) return 0;
    if (*a == 0 && *b == 0) return 0;
    if (*a != *b) return 0 + *a - *b;
    a++;
    b++;
  }
}

int GetUtf8Length(v8::Isolate* isolate, Local<String> str) {
  int len = str->Utf8Length(isolate);
  if (len < 0) {
    i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
    i::Handle<i::String> istr(v8::Utils::OpenHandle(*str));
    i::String::Flatten(i_isolate, istr);
    len = str->Utf8Length(isolate);
  }
  return len;
}


THREADED_TEST(StringWrite) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<String> str = v8_str("abcde");
  // abc<Icelandic eth><Unicode snowman>.
  v8::Local<String> str2 = v8_str("abc\xC3\xB0\xE2\x98\x83");
  v8::Local<String> str3 =
      v8::String::NewFromUtf8(context->GetIsolate(), "abc\0def",
                              v8::NewStringType::kNormal, 7)
          .ToLocalChecked();
  // "ab" + lead surrogate + "wx" + trail surrogate + "yz"
  uint16_t orphans[8] = {0x61, 0x62, 0xD800, 0x77, 0x78, 0xDC00, 0x79, 0x7A};
  v8::Local<String> orphans_str =
      v8::String::NewFromTwoByte(context->GetIsolate(), orphans,
                                 v8::NewStringType::kNormal, 8)
          .ToLocalChecked();
  // single lead surrogate
  uint16_t lead[1] = {0xD800};
  v8::Local<String> lead_str =
      v8::String::NewFromTwoByte(context->GetIsolate(), lead,
                                 v8::NewStringType::kNormal, 1)
          .ToLocalChecked();
  // single trail surrogate
  uint16_t trail[1] = {0xDC00};
  v8::Local<String> trail_str =
      v8::String::NewFromTwoByte(context->GetIsolate(), trail,
                                 v8::NewStringType::kNormal, 1)
          .ToLocalChecked();
  // surrogate pair
  uint16_t pair[2] = {0xD800, 0xDC00};
  v8::Local<String> pair_str =
      v8::String::NewFromTwoByte(context->GetIsolate(), pair,
                                 v8::NewStringType::kNormal, 2)
          .ToLocalChecked();
  const int kStride = 4;  // Must match stride in for loops in JS below.
  CompileRun(
      "var left = '';"
      "for (var i = 0; i < 0xD800; i += 4) {"
      "  left = left + String.fromCharCode(i);"
      "}");
  CompileRun(
      "var right = '';"
      "for (var i = 0; i < 0xD800; i += 4) {"
      "  right = String.fromCharCode(i) + right;"
      "}");
  v8::Local<v8::Object> global = context->Global();
  Local<String> left_tree = global->Get(context.local(), v8_str("left"))
                                .ToLocalChecked()
                                .As<String>();
  Local<String> right_tree = global->Get(context.local(), v8_str("right"))
                                 .ToLocalChecked()
                                 .As<String>();

  CHECK_EQ(5, str2->Length());
  CHECK_EQ(0xD800 / kStride, left_tree->Length());
  CHECK_EQ(0xD800 / kStride, right_tree->Length());

  char buf[100];
  char utf8buf[0xD800 * 3];
  uint16_t wbuf[100];
  int len;
  int charlen;

  memset(utf8buf, 0x1, 1000);
  len = v8::String::Empty(isolate)->WriteUtf8(isolate, utf8buf, sizeof(utf8buf),
                                              &charlen);
  CHECK_EQ(1, len);
  CHECK_EQ(0, charlen);
  CHECK_EQ(0, strcmp(utf8buf, ""));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen);
  CHECK_EQ(9, len);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "abc\xC3\xB0\xE2\x98\x83"));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 8, &charlen);
  CHECK_EQ(8, len);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\xC3\xB0\xE2\x98\x83\x01", 9));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 7, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\xC3\xB0\x01", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 6, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\xC3\xB0\x01", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 5, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\xC3\xB0\x01", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 4, &charlen);
  CHECK_EQ(3, len);
  CHECK_EQ(3, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\x01", 4));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 3, &charlen);
  CHECK_EQ(3, len);
  CHECK_EQ(3, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\x01", 4));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(isolate, utf8buf, 2, &charlen);
  CHECK_EQ(2, len);
  CHECK_EQ(2, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "ab\x01", 3));

  // allow orphan surrogates by default
  memset(utf8buf, 0x1, 1000);
  len = orphans_str->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen);
  CHECK_EQ(13, len);
  CHECK_EQ(8, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "ab\xED\xA0\x80wx\xED\xB0\x80yz"));

  // replace orphan surrogates with Unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = orphans_str->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen,
                               String::REPLACE_INVALID_UTF8);
  CHECK_EQ(13, len);
  CHECK_EQ(8, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "ab\xEF\xBF\xBDwx\xEF\xBF\xBDyz"));

  // replace single lead surrogate with Unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = lead_str->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen,
                            String::REPLACE_INVALID_UTF8);
  CHECK_EQ(4, len);
  CHECK_EQ(1, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "\xEF\xBF\xBD"));

  // replace single trail surrogate with Unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = trail_str->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen,
                             String::REPLACE_INVALID_UTF8);
  CHECK_EQ(4, len);
  CHECK_EQ(1, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "\xEF\xBF\xBD"));

  // do not replace / write anything if surrogate pair does not fit the buffer
  // space
  memset(utf8buf, 0x1, 1000);
  len = pair_str->WriteUtf8(isolate, utf8buf, 3, &charlen,
                            String::REPLACE_INVALID_UTF8);
  CHECK_EQ(0, len);
  CHECK_EQ(0, charlen);

  memset(utf8buf, 0x1, sizeof(utf8buf));
  len = GetUtf8Length(isolate, left_tree);
  int utf8_expected =
      (0x80 + (0x800 - 0x80) * 2 + (0xD800 - 0x800) * 3) / kStride;
  CHECK_EQ(utf8_expected, len);
  len = left_tree->WriteUtf8(isolate, utf8buf, utf8_expected, &charlen);
  CHECK_EQ(utf8_expected, len);
  CHECK_EQ(0xD800 / kStride, charlen);
  CHECK_EQ(0xED, static_cast<unsigned char>(utf8buf[utf8_expected - 3]));
  CHECK_EQ(0x9F, static_cast<unsigned char>(utf8buf[utf8_expected - 2]));
  CHECK_EQ(0xC0 - kStride,
           static_cast<unsigned char>(utf8buf[utf8_expected - 1]));
  CHECK_EQ(1, utf8buf[utf8_expected]);

  memset(utf8buf, 0x1, sizeof(utf8buf));
  len = GetUtf8Length(isolate, right_tree);
  CHECK_EQ(utf8_expected, len);
  len = right_tree->WriteUtf8(isolate, utf8buf, utf8_expected, &charlen);
  CHECK_EQ(utf8_expected, len);
  CHECK_EQ(0xD800 / kStride, charlen);
  CHECK_EQ(0xED, static_cast<unsigned char>(utf8buf[0]));
  CHECK_EQ(0x9F, static_cast<unsigned char>(utf8buf[1]));
  CHECK_EQ(0xC0 - kStride, static_cast<unsigned char>(utf8buf[2]));
  CHECK_EQ(1, utf8buf[utf8_expected]);

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf));
  CHECK_EQ(5, len);
  len = str->Write(isolate, wbuf);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strcmp("abcde", buf));
  uint16_t answer1[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer1, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 0, 4);
  CHECK_EQ(4, len);
  len = str->Write(isolate, wbuf, 0, 4);
  CHECK_EQ(4, len);
  CHECK_EQ(0, strncmp("abcd\x01", buf, 5));
  uint16_t answer2[] = {'a', 'b', 'c', 'd', 0x101};
  CHECK_EQ(0, StrNCmp16(answer2, wbuf, 5));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 0, 5);
  CHECK_EQ(5, len);
  len = str->Write(isolate, wbuf, 0, 5);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strncmp("abcde\x01", buf, 6));
  uint16_t answer3[] = {'a', 'b', 'c', 'd', 'e', 0x101};
  CHECK_EQ(0, StrNCmp16(answer3, wbuf, 6));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 0, 6);
  CHECK_EQ(5, len);
  len = str->Write(isolate, wbuf, 0, 6);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strcmp("abcde", buf));
  uint16_t answer4[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer4, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 4, -1);
  CHECK_EQ(1, len);
  len = str->Write(isolate, wbuf, 4, -1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strcmp("e", buf));
  uint16_t answer5[] = {'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer5, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 4, 6);
  CHECK_EQ(1, len);
  len = str->Write(isolate, wbuf, 4, 6);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strcmp("e", buf));
  CHECK_EQ(0, StrCmp16(answer5, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 4, 1);
  CHECK_EQ(1, len);
  len = str->Write(isolate, wbuf, 4, 1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strncmp("e\x01", buf, 2));
  uint16_t answer6[] = {'e', 0x101};
  CHECK_EQ(0, StrNCmp16(answer6, wbuf, 2));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 3, 1);
  CHECK_EQ(1, len);
  len = str->Write(isolate, wbuf, 3, 1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strncmp("d\x01", buf, 2));
  uint16_t answer7[] = {'d', 0x101};
  CHECK_EQ(0, StrNCmp16(answer7, wbuf, 2));

  memset(wbuf, 0x1, sizeof(wbuf));
  wbuf[5] = 'X';
  len = str->Write(isolate, wbuf, 0, 6, String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', wbuf[5]);
  uint16_t answer8a[] = {'a', 'b', 'c', 'd', 'e'};
  uint16_t answer8b[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrNCmp16(answer8a, wbuf, 5));
  CHECK_NE(0, StrCmp16(answer8b, wbuf));
  wbuf[5] = '\0';
  CHECK_EQ(0, StrCmp16(answer8b, wbuf));

  memset(buf, 0x1, sizeof(buf));
  buf[5] = 'X';
  len = str->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf), 0, 6,
                          String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', buf[5]);
  CHECK_EQ(0, strncmp("abcde", buf, 5));
  CHECK_NE(0, strcmp("abcde", buf));
  buf[5] = '\0';
  CHECK_EQ(0, strcmp("abcde", buf));

  memset(utf8buf, 0x1, sizeof(utf8buf));
  utf8buf[8] = 'X';
  len = str2->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen,
                        String::NO_NULL_TERMINATION);
  CHECK_EQ(8, len);
  CHECK_EQ('X', utf8buf[8]);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\xC3\xB0\xE2\x98\x83", 8));
  CHECK_NE(0, strcmp(utf8buf, "abc\xC3\xB0\xE2\x98\x83"));
  utf8buf[8] = '\0';
  CHECK_EQ(0, strcmp(utf8buf, "abc\xC3\xB0\xE2\x98\x83"));

  memset(utf8buf, 0x1, sizeof(utf8buf));
  utf8buf[5] = 'X';
  len = str->WriteUtf8(isolate, utf8buf, sizeof(utf8buf), &charlen,
                       String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', utf8buf[5]);  // Test that the sixth character is untouched.
  CHECK_EQ(5, charlen);
  utf8buf[5] = '\0';
  CHECK_EQ(0, strcmp(utf8buf, "abcde"));

  memset(buf, 0x1, sizeof(buf));
  len = str3->WriteOneByte(isolate, reinterpret_cast<uint8_t*>(buf));
  CHECK_EQ(7, len);
  CHECK_EQ(0, strcmp("abc", buf));
  CHECK_EQ(0, buf[3]);
  CHECK_EQ(0, strcmp("def", buf + 4));

  CHECK_EQ(0, str->WriteOneByte(isolate, nullptr, 0, 0,
                                String::NO_NULL_TERMINATION));
  CHECK_EQ(0, str->WriteUtf8(isolate, nullptr, 0, nullptr,
                             String::NO_NULL_TERMINATION));
  CHECK_EQ(0, str->Write(isolate, nullptr, 0, 0, String::NO_NULL_TERMINATION));
}


static void Utf16Helper(
    LocalContext& context,  // NOLINT
    const char* name,
    const char* lengths_name,
    int len) {
  Local<v8::Array> a = Local<v8::Array>::Cast(
      context->Global()->Get(context.local(), v8_str(name)).ToLocalChecked());
  Local<v8::Array> alens =
      Local<v8::Array>::Cast(context->Global()
                                 ->Get(context.local(), v8_str(lengths_name))
                                 .ToLocalChecked());
  for (int i = 0; i < len; i++) {
    Local<v8::String> string =
        Local<v8::String>::Cast(a->Get(context.local(), i).ToLocalChecked());
    Local<v8::Number> expected_len = Local<v8::Number>::Cast(
        alens->Get(context.local(), i).ToLocalChecked());
    int length = GetUtf8Length(context->GetIsolate(), string);
    CHECK_EQ(static_cast<int>(expected_len->Value()), length);
  }
}

void TestUtf8DecodingAgainstReference(
    v8::Isolate* isolate, const char* cases[],
    const std::vector<std::vector<uint16_t>>& unicode_expected) {
  for (size_t test_ix = 0; test_ix < unicode_expected.size(); ++test_ix) {
    v8::Local<String> str = v8_str(cases[test_ix]);
    CHECK_EQ(unicode_expected[test_ix].size(), str->Length());

    std::unique_ptr<uint16_t[]> buffer(new uint16_t[str->Length()]);
    str->Write(isolate, buffer.get(), 0, -1, String::NO_NULL_TERMINATION);

    for (size_t i = 0; i < unicode_expected[test_ix].size(); ++i) {
      CHECK_EQ(unicode_expected[test_ix][i], buffer[i]);
    }
  }
}

THREADED_TEST(OverlongSequencesAndSurrogates) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* cases[] = {
      // Overlong 2-byte sequence.
      "X\xc0\xbfY\0",
      // Another overlong 2-byte sequence.
      "X\xc1\xbfY\0",
      // Overlong 3-byte sequence.
      "X\xe0\x9f\xbfY\0",
      // Overlong 4-byte sequence.
      "X\xf0\x89\xbf\xbfY\0",
      // Invalid 3-byte sequence (reserved for surrogates).
      "X\xed\xa0\x80Y\0",
      // Invalid 4-bytes sequence (value out of range).
      "X\xf4\x90\x80\x80Y\0",

      // Start of an overlong 3-byte sequence but not enough continuation bytes.
      "X\xe0\x9fY\0",
      // Start of an overlong 4-byte sequence but not enough continuation bytes.
      "X\xf0\x89\xbfY\0",
      // Start of an invalid 3-byte sequence (reserved for surrogates) but not
      // enough continuation bytes.
      "X\xed\xa0Y\0",
      // Start of an invalid 4-bytes sequence (value out of range) but not
      // enough continuation bytes.
      "X\xf4\x90\x80Y\0",
  };
  const std::vector<std::vector<uint16_t>> unicode_expected = {
      {0x58, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0x59},
      {0x58, 0xFFFD, 0xFFFD, 0xFFFD, 0x59},
  };
  CHECK_EQ(unicode_expected.size(), arraysize(cases));
  TestUtf8DecodingAgainstReference(context->GetIsolate(), cases,
                                   unicode_expected);
}

THREADED_TEST(Utf16) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  CompileRun(
      "var pad = '01234567890123456789';"
      "var p = [];"
      "var plens = [20, 3, 3];"
      "p.push('01234567890123456789');"
      "var lead = 0xD800;"
      "var trail = 0xDC00;"
      "p.push(String.fromCharCode(0xD800));"
      "p.push(String.fromCharCode(0xDC00));"
      "var a = [];"
      "var b = [];"
      "var c = [];"
      "var alens = [];"
      "for (var i = 0; i < 3; i++) {"
      "  p[1] = String.fromCharCode(lead++);"
      "  for (var j = 0; j < 3; j++) {"
      "    p[2] = String.fromCharCode(trail++);"
      "    a.push(p[i] + p[j]);"
      "    b.push(p[i] + p[j]);"
      "    c.push(p[i] + p[j]);"
      "    alens.push(plens[i] + plens[j]);"
      "  }"
      "}"
      "alens[5] -= 2;"  // Here the surrogate pairs match up.
      "var a2 = [];"
      "var b2 = [];"
      "var c2 = [];"
      "var a2lens = [];"
      "for (var m = 0; m < 9; m++) {"
      "  for (var n = 0; n < 9; n++) {"
      "    a2.push(a[m] + a[n]);"
      "    b2.push(b[m] + b[n]);"
      "    var newc = 'x' + c[m] + c[n] + 'y';"
      "    c2.push(newc.substring(1, newc.length - 1));"
      "    var utf = alens[m] + alens[n];"  // And here.
                                            // The 'n's that start with 0xDC..
                                            // are 6-8 The 'm's that end with
                                            // 0xD8.. are 1, 4 and 7
      "    if ((m % 3) == 1 && n >= 6) utf -= 2;"
      "    a2lens.push(utf);"
      "  }"
      "}");
  Utf16Helper(context, "a", "alens", 9);
  Utf16Helper(context, "a2", "a2lens", 81);
}


static bool SameSymbol(Local<String> s1, Local<String> s2) {
  i::Handle<i::String> is1(v8::Utils::OpenHandle(*s1));
  i::Handle<i::String> is2(v8::Utils::OpenHandle(*s2));
  return *is1 == *is2;
}


THREADED_TEST(Utf16Symbol) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<String> symbol1 =
      v8::String::NewFromUtf8(context->GetIsolate(), "abc",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  Local<String> symbol2 =
      v8::String::NewFromUtf8(context->GetIsolate(), "abc",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  CHECK(SameSymbol(symbol1, symbol2));

  CompileRun(
      "var sym0 = 'benedictus';"
      "var sym0b = 'S\xC3\xB8ren';"
      "var sym1 = '\xED\xA0\x81\xED\xB0\x87';"
      "var sym2 = '\xF0\x90\x90\x88';"
      "var sym3 = 'x\xED\xA0\x81\xED\xB0\x87';"
      "var sym4 = 'x\xF0\x90\x90\x88';"
      "if (sym1.length != 2) throw sym1;"
      "if (sym1.charCodeAt(1) != 0xDC07) throw sym1.charCodeAt(1);"
      "if (sym2.length != 2) throw sym2;"
      "if (sym2.charCodeAt(1) != 0xDC08) throw sym2.charCodeAt(2);"
      "if (sym3.length != 3) throw sym3;"
      "if (sym3.charCodeAt(2) != 0xDC07) throw sym1.charCodeAt(2);"
      "if (sym4.length != 3) throw sym4;"
      "if (sym4.charCodeAt(2) != 0xDC08) throw sym2.charCodeAt(2);");
  Local<String> sym0 =
      v8::String::NewFromUtf8(context->GetIsolate(), "benedictus",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  Local<String> sym0b =
      v8::String::NewFromUtf8(context->GetIsolate(), "S\xC3\xB8ren",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  Local<String> sym1 =
      v8::String::NewFromUtf8(context->GetIsolate(), "\xED\xA0\x81\xED\xB0\x87",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  Local<String> sym2 =
      v8::String::NewFromUtf8(context->GetIsolate(), "\xF0\x90\x90\x88",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  Local<String> sym3 = v8::String::NewFromUtf8(context->GetIsolate(),
                                               "x\xED\xA0\x81\xED\xB0\x87",
                                               v8::NewStringType::kInternalized)
                           .ToLocalChecked();
  Local<String> sym4 =
      v8::String::NewFromUtf8(context->GetIsolate(), "x\xF0\x90\x90\x88",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  v8::Local<v8::Object> global = context->Global();
  Local<Value> s0 =
      global->Get(context.local(), v8_str("sym0")).ToLocalChecked();
  Local<Value> s0b =
      global->Get(context.local(), v8_str("sym0b")).ToLocalChecked();
  Local<Value> s1 =
      global->Get(context.local(), v8_str("sym1")).ToLocalChecked();
  Local<Value> s2 =
      global->Get(context.local(), v8_str("sym2")).ToLocalChecked();
  Local<Value> s3 =
      global->Get(context.local(), v8_str("sym3")).ToLocalChecked();
  Local<Value> s4 =
      global->Get(context.local(), v8_str("sym4")).ToLocalChecked();
  CHECK(SameSymbol(sym0, Local<String>::Cast(s0)));
  CHECK(SameSymbol(sym0b, Local<String>::Cast(s0b)));
  CHECK(SameSymbol(sym1, Local<String>::Cast(s1)));
  CHECK(SameSymbol(sym2, Local<String>::Cast(s2)));
  CHECK(SameSymbol(sym3, Local<String>::Cast(s3)));
  CHECK(SameSymbol(sym4, Local<String>::Cast(s4)));
}


THREADED_TEST(Utf16MissingTrailing) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Make sure it will go past the buffer, so it will call `WriteUtf16Slow`
  int size = 1024 * 64;
  uint8_t* buffer = new uint8_t[size];
  for (int i = 0; i < size; i += 4) {
    buffer[i] = 0xF0;
    buffer[i + 1] = 0x9D;
    buffer[i + 2] = 0x80;
    buffer[i + 3] = 0x9E;
  }

  // Now invoke the decoder without last 3 bytes
  v8::Local<v8::String> str =
      v8::String::NewFromUtf8(
          context->GetIsolate(), reinterpret_cast<char*>(buffer),
          v8::NewStringType::kNormal, size - 3).ToLocalChecked();
  USE(str);
  delete[] buffer;
}


THREADED_TEST(Utf16Trailing3Byte) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  // Make sure it will go past the buffer, so it will call `WriteUtf16Slow`
  int size = 1024 * 63;
  uint8_t* buffer = new uint8_t[size];
  for (int i = 0; i < size; i += 3) {
    buffer[i] = 0xE2;
    buffer[i + 1] = 0x80;
    buffer[i + 2] = 0xA6;
  }

  // Now invoke the decoder without last 3 bytes
  v8::Local<v8::String> str =
      v8::String::NewFromUtf8(isolate, reinterpret_cast<char*>(buffer),
                              v8::NewStringType::kNormal, size)
          .ToLocalChecked();

  v8::String::Value value(isolate, str);
  CHECK_EQ(value.length(), size / 3);
  CHECK_EQ((*value)[value.length() - 1], 0x2026);

  delete[] buffer;
}


THREADED_TEST(ToArrayIndex) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> str = v8_str("42");
  v8::MaybeLocal<v8::Uint32> index = str->ToArrayIndex(context.local());
  CHECK(!index.IsEmpty());
  CHECK_EQ(42.0,
           index.ToLocalChecked()->Uint32Value(context.local()).FromJust());
  str = v8_str("42asdf");
  index = str->ToArrayIndex(context.local());
  CHECK(index.IsEmpty());
  str = v8_str("-42");
  index = str->ToArrayIndex(context.local());
  CHECK(index.IsEmpty());
  str = v8_str("4294967294");
  index = str->ToArrayIndex(context.local());
  CHECK(!index.IsEmpty());
  CHECK_EQ(4294967294.0,
           index.ToLocalChecked()->Uint32Value(context.local()).FromJust());
  v8::Local<v8::Number> num = v8::Number::New(isolate, 1);
  index = num->ToArrayIndex(context.local());
  CHECK(!index.IsEmpty());
  CHECK_EQ(1.0,
           index.ToLocalChecked()->Uint32Value(context.local()).FromJust());
  num = v8::Number::New(isolate, -1);
  index = num->ToArrayIndex(context.local());
  CHECK(index.IsEmpty());
  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  index = obj->ToArrayIndex(context.local());
  CHECK(index.IsEmpty());
}

THREADED_TEST(ErrorConstruction) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  v8::Local<String> foo = v8_str("foo");
  v8::Local<String> message = v8_str("message");
  v8::Local<Value> range_error = v8::Exception::RangeError(foo);
  CHECK(range_error->IsObject());
  CHECK(range_error.As<v8::Object>()
            ->Get(context.local(), message)
            .ToLocalChecked()
            ->Equals(context.local(), foo)
            .FromJust());
  v8::Local<Value> reference_error = v8::Exception::ReferenceError(foo);
  CHECK(reference_error->IsObject());
  CHECK(reference_error.As<v8::Object>()
            ->Get(context.local(), message)
            .ToLocalChecked()
            ->Equals(context.local(), foo)
            .FromJust());
  v8::Local<Value> syntax_error = v8::Exception::SyntaxError(foo);
  CHECK(syntax_error->IsObject());
  CHECK(syntax_error.As<v8::Object>()
            ->Get(context.local(), message)
            .ToLocalChecked()
            ->Equals(context.local(), foo)
            .FromJust());
  v8::Local<Value> type_error = v8::Exception::TypeError(foo);
  CHECK(type_error->IsObject());
  CHECK(type_error.As<v8::Object>()
            ->Get(context.local(), message)
            .ToLocalChecked()
            ->Equals(context.local(), foo)
            .FromJust());
  v8::Local<Value> error = v8::Exception::Error(foo);
  CHECK(error->IsObject());
  CHECK(error.As<v8::Object>()
            ->Get(context.local(), message)
            .ToLocalChecked()
            ->Equals(context.local(), foo)
            .FromJust());
}

THREADED_TEST(ExceptionCreateMessageLength) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Test that the message is not truncated.
  TryCatch try_catch(context->GetIsolate());
  CompileRun(
      "var message = 'm';"
      "while (message.length < 1000) message += message;"
      "throw message;");
  CHECK(try_catch.HasCaught());

  CHECK_LT(1000, try_catch.Message()->Get()->Length());
}


static void YGetter(Local<String> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(10));
}


static void YSetter(Local<String> name,
                    Local<Value> value,
                    const v8::PropertyCallbackInfo<void>& info) {
  Local<Object> this_obj = Local<Object>::Cast(info.This());
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  if (this_obj->Has(context, name).FromJust())
    this_obj->Delete(context, name).FromJust();
  CHECK(this_obj->Set(context, name, value).FromJust());
}


THREADED_TEST(DeleteAccessor) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("y"), YGetter, YSetter);
  LocalContext context;
  v8::Local<v8::Object> holder =
      obj->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("holder"), holder)
            .FromJust());
  v8::Local<Value> result =
      CompileRun("holder.y = 11; holder.y = 12; holder.y");
  CHECK_EQ(12u, result->Uint32Value(context.local()).FromJust());
}


static int trouble_nesting = 0;
static void TroubleCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  trouble_nesting++;

  // Call a JS function that throws an uncaught exception.
  Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
  Local<v8::Object> arg_this = context->Global();
  Local<Value> trouble_callee =
      (trouble_nesting == 3)
          ? arg_this->Get(context, v8_str("trouble_callee")).ToLocalChecked()
          : arg_this->Get(context, v8_str("trouble_caller")).ToLocalChecked();
  CHECK(trouble_callee->IsFunction());
  args.GetReturnValue().Set(Function::Cast(*trouble_callee)
                                ->Call(context, arg_this, 0, nullptr)
                                .FromMaybe(v8::Local<v8::Value>()));
}


static int report_count = 0;
static void ApiUncaughtExceptionTestListener(v8::Local<v8::Message>,
                                             v8::Local<Value>) {
  report_count++;
}


// Counts uncaught exceptions, but other tests running in parallel
// also have uncaught exceptions.
TEST(ApiUncaughtException) {
  report_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  isolate->AddMessageListener(ApiUncaughtExceptionTestListener);

  Local<v8::FunctionTemplate> fun =
      v8::FunctionTemplate::New(isolate, TroubleCallback);
  v8::Local<v8::Object> global = env->Global();
  CHECK(global->Set(env.local(), v8_str("trouble"),
                    fun->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  CompileRun(
      "function trouble_callee() {"
      "  var x = null;"
      "  return x.foo;"
      "};"
      "function trouble_caller() {"
      "  trouble();"
      "};");
  Local<Value> trouble =
      global->Get(env.local(), v8_str("trouble")).ToLocalChecked();
  CHECK(trouble->IsFunction());
  Local<Value> trouble_callee =
      global->Get(env.local(), v8_str("trouble_callee")).ToLocalChecked();
  CHECK(trouble_callee->IsFunction());
  Local<Value> trouble_caller =
      global->Get(env.local(), v8_str("trouble_caller")).ToLocalChecked();
  CHECK(trouble_caller->IsFunction());
  Function::Cast(*trouble_caller)
      ->Call(env.local(), global, 0, nullptr)
      .FromMaybe(v8::Local<v8::Value>());
  CHECK_EQ(1, report_count);
  isolate->RemoveMessageListeners(ApiUncaughtExceptionTestListener);
}


static const char* script_resource_name = "ExceptionInNativeScript.js";
static void ExceptionInNativeScriptTestListener(v8::Local<v8::Message> message,
                                                v8::Local<Value>) {
  v8::Isolate* isolate = message->GetIsolate();
  v8::Local<v8::Value> name_val = message->GetScriptOrigin().ResourceName();
  CHECK(!name_val.IsEmpty() && name_val->IsString());
  v8::String::Utf8Value name(isolate,
                             message->GetScriptOrigin().ResourceName());
  CHECK_EQ(0, strcmp(script_resource_name, *name));
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  CHECK_EQ(3, message->GetLineNumber(context).FromJust());
  v8::String::Utf8Value source_line(
      isolate, message->GetSourceLine(context).ToLocalChecked());
  CHECK_EQ(0, strcmp("  new o.foo();", *source_line));
}


TEST(ExceptionInNativeScript) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  isolate->AddMessageListener(ExceptionInNativeScriptTestListener);

  Local<v8::FunctionTemplate> fun =
      v8::FunctionTemplate::New(isolate, TroubleCallback);
  v8::Local<v8::Object> global = env->Global();
  CHECK(global->Set(env.local(), v8_str("trouble"),
                    fun->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  CompileRunWithOrigin(
      "function trouble() {\n"
      "  var o = {};\n"
      "  new o.foo();\n"
      "};",
      script_resource_name);
  Local<Value> trouble =
      global->Get(env.local(), v8_str("trouble")).ToLocalChecked();
  CHECK(trouble->IsFunction());
  CHECK(Function::Cast(*trouble)
            ->Call(env.local(), global, 0, nullptr)
            .IsEmpty());
  isolate->RemoveMessageListeners(ExceptionInNativeScriptTestListener);
}


TEST(CompilationErrorUsingTryCatchHandler) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::TryCatch try_catch(env->GetIsolate());
  v8_compile("This doesn't &*&@#$&*^ compile.");
  CHECK(*try_catch.Exception());
  CHECK(try_catch.HasCaught());
}


TEST(TryCatchFinallyUsingTryCatchHandler) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::TryCatch try_catch(env->GetIsolate());
  CompileRun("try { throw ''; } catch (e) {}");
  CHECK(!try_catch.HasCaught());
  CompileRun("try { throw ''; } finally {}");
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CompileRun(
      "(function() {"
      "try { throw ''; } finally { return; }"
      "})()");
  CHECK(!try_catch.HasCaught());
  CompileRun(
      "(function()"
      "  { try { throw ''; } finally { throw 0; }"
      "})()");
  CHECK(try_catch.HasCaught());
}


void CEvaluate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  CompileRun(args[0]
                 ->ToString(args.GetIsolate()->GetCurrentContext())
                 .ToLocalChecked());
}


TEST(TryCatchFinallyStoresMessageUsingTryCatchHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("CEvaluate"),
             v8::FunctionTemplate::New(isolate, CEvaluate));
  LocalContext context(nullptr, templ);
  v8::TryCatch try_catch(isolate);
  CompileRun("try {"
             "  CEvaluate('throw 1;');"
             "} finally {"
             "}");
  CHECK(try_catch.HasCaught());
  CHECK(!try_catch.Message().IsEmpty());
  String::Utf8Value exception_value(isolate, try_catch.Exception());
  CHECK_EQ(0, strcmp(*exception_value, "1"));
  try_catch.Reset();
  CompileRun("try {"
             "  CEvaluate('throw 1;');"
             "} finally {"
             "  throw 2;"
             "}");
  CHECK(try_catch.HasCaught());
  CHECK(!try_catch.Message().IsEmpty());
  String::Utf8Value finally_exception_value(isolate, try_catch.Exception());
  CHECK_EQ(0, strcmp(*finally_exception_value, "2"));
}


// For use within the TestSecurityHandler() test.
static bool g_security_callback_result = false;
static bool SecurityTestCallback(Local<v8::Context> accessing_context,
                                 Local<v8::Object> accessed_object,
                                 Local<v8::Value> data) {
  printf("a\n");
  CHECK(!data.IsEmpty() && data->IsInt32());
  CHECK_EQ(42, data->Int32Value(accessing_context).FromJust());
  return g_security_callback_result;
}


// SecurityHandler can't be run twice
TEST(SecurityHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope0(isolate);
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallback(SecurityTestCallback, v8_num(42));
  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, nullptr, global_template);
  context0->Enter();

  v8::Local<v8::Object> global0 = context0->Global();
  v8::Local<Script> script0 = v8_compile("foo = 111");
  script0->Run(context0).ToLocalChecked();
  CHECK(global0->Set(context0, v8_str("0"), v8_num(999)).FromJust());
  v8::Local<Value> foo0 =
      global0->Get(context0, v8_str("foo")).ToLocalChecked();
  CHECK_EQ(111, foo0->Int32Value(context0).FromJust());
  v8::Local<Value> z0 = global0->Get(context0, v8_str("0")).ToLocalChecked();
  CHECK_EQ(999, z0->Int32Value(context0).FromJust());

  // Create another environment, should fail security checks.
  v8::HandleScope scope1(isolate);

  v8::Local<Context> context1 = Context::New(isolate, nullptr, global_template);
  context1->Enter();

  v8::Local<v8::Object> global1 = context1->Global();
  global1->Set(context1, v8_str("othercontext"), global0).FromJust();
  // This set will fail the security check.
  v8::Local<Script> script1 =
      v8_compile("othercontext.foo = 222; othercontext[0] = 888;");
  CHECK(script1->Run(context1).IsEmpty());
  g_security_callback_result = true;
  // This read will pass the security check.
  v8::Local<Value> foo1 =
      global0->Get(context1, v8_str("foo")).ToLocalChecked();
  CHECK_EQ(111, foo1->Int32Value(context0).FromJust());
  // This read will pass the security check.
  v8::Local<Value> z1 = global0->Get(context1, v8_str("0")).ToLocalChecked();
  CHECK_EQ(999, z1->Int32Value(context1).FromJust());

  // Create another environment, should pass security checks.
  {
    v8::HandleScope scope2(isolate);
    LocalContext context2;
    v8::Local<v8::Object> global2 = context2->Global();
    CHECK(global2->Set(context2.local(), v8_str("othercontext"), global0)
              .FromJust());
    v8::Local<Script> script2 =
        v8_compile("othercontext.foo = 333; othercontext[0] = 888;");
    script2->Run(context2.local()).ToLocalChecked();
    v8::Local<Value> foo2 =
        global0->Get(context2.local(), v8_str("foo")).ToLocalChecked();
    CHECK_EQ(333, foo2->Int32Value(context2.local()).FromJust());
    v8::Local<Value> z2 =
        global0->Get(context2.local(), v8_str("0")).ToLocalChecked();
    CHECK_EQ(888, z2->Int32Value(context2.local()).FromJust());
  }

  context1->Exit();
  context0->Exit();
}


THREADED_TEST(SecurityChecks) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);

  // Create a function in env1.
  CompileRun("spy=function(){return spy;}");
  Local<Value> spy =
      env1->Global()->Get(env1.local(), v8_str("spy")).ToLocalChecked();
  CHECK(spy->IsFunction());

  // Create another function accessing global objects.
  CompileRun("spy2=function(){return new this.Array();}");
  Local<Value> spy2 =
      env1->Global()->Get(env1.local(), v8_str("spy2")).ToLocalChecked();
  CHECK(spy2->IsFunction());

  // Switch to env2 in the same domain and invoke spy on env2.
  {
    env2->SetSecurityToken(foo);
    // Enter env2
    Context::Scope scope_env2(env2);
    Local<Value> result = Function::Cast(*spy)
                              ->Call(env2, env2->Global(), 0, nullptr)
                              .ToLocalChecked();
    CHECK(result->IsFunction());
  }

  {
    env2->SetSecurityToken(bar);
    Context::Scope scope_env2(env2);

    // Call cross_domain_call, it should throw an exception
    v8::TryCatch try_catch(env1->GetIsolate());
    CHECK(Function::Cast(*spy2)
              ->Call(env2, env2->Global(), 0, nullptr)
              .IsEmpty());
    CHECK(try_catch.HasCaught());
  }
}


// Regression test case for issue 1183439.
THREADED_TEST(SecurityChecksForPrototypeChain) {
  LocalContext current;
  v8::HandleScope scope(current->GetIsolate());
  v8::Local<Context> other = Context::New(current->GetIsolate());

  // Change context to be able to get to the Object function in the
  // other context without hitting the security checks.
  v8::Local<Value> other_object;
  { Context::Scope scope(other);
    other_object =
        other->Global()->Get(other, v8_str("Object")).ToLocalChecked();
    CHECK(other->Global()->Set(other, v8_num(42), v8_num(87)).FromJust());
  }

  CHECK(current->Global()
            ->Set(current.local(), v8_str("other"), other->Global())
            .FromJust());
  CHECK(v8_compile("other")
            ->Run(current.local())
            .ToLocalChecked()
            ->Equals(current.local(), other->Global())
            .FromJust());

  // Make sure the security check fails here and we get an undefined
  // result instead of getting the Object function. Repeat in a loop
  // to make sure to exercise the IC code.
  v8::Local<Script> access_other0 = v8_compile("other.Object");
  v8::Local<Script> access_other1 = v8_compile("other[42]");
  for (int i = 0; i < 5; i++) {
    CHECK(access_other0->Run(current.local()).IsEmpty());
    CHECK(access_other1->Run(current.local()).IsEmpty());
  }

  // Create an object that has 'other' in its prototype chain and make
  // sure we cannot access the Object function indirectly through
  // that. Repeat in a loop to make sure to exercise the IC code.
  v8_compile(
      "function F() { };"
      "F.prototype = other;"
      "var f = new F();")
      ->Run(current.local())
      .ToLocalChecked();
  v8::Local<Script> access_f0 = v8_compile("f.Object");
  v8::Local<Script> access_f1 = v8_compile("f[42]");
  for (int j = 0; j < 5; j++) {
    CHECK(access_f0->Run(current.local()).IsEmpty());
    CHECK(access_f1->Run(current.local()).IsEmpty());
  }

  // Now it gets hairy: Set the prototype for the other global object
  // to be the current global object. The prototype chain for 'f' now
  // goes through 'other' but ends up in the current global object.
  { Context::Scope scope(other);
    CHECK(other->Global()
              ->Set(other, v8_str("__proto__"), current->Global())
              .FromJust());
  }
  // Set a named and an index property on the current global
  // object. To force the lookup to go through the other global object,
  // the properties must not exist in the other global object.
  CHECK(current->Global()
            ->Set(current.local(), v8_str("foo"), v8_num(100))
            .FromJust());
  CHECK(current->Global()
            ->Set(current.local(), v8_num(99), v8_num(101))
            .FromJust());
  // Try to read the properties from f and make sure that the access
  // gets stopped by the security checks on the other global object.
  Local<Script> access_f2 = v8_compile("f.foo");
  Local<Script> access_f3 = v8_compile("f[99]");
  for (int k = 0; k < 5; k++) {
    CHECK(access_f2->Run(current.local()).IsEmpty());
    CHECK(access_f3->Run(current.local()).IsEmpty());
  }
}


static bool security_check_with_gc_called;

static bool SecurityTestCallbackWithGC(Local<v8::Context> accessing_context,
                                       Local<v8::Object> accessed_object,
                                       Local<v8::Value> data) {
  CcTest::CollectAllGarbage();
  security_check_with_gc_called = true;
  return true;
}


TEST(SecurityTestGCAllowed) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(SecurityTestCallbackWithGC);

  v8::Local<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  CHECK(context->Global()
            ->Set(context, v8_str("obj"),
                  object_template->NewInstance(context).ToLocalChecked())
            .FromJust());

  security_check_with_gc_called = false;
  CompileRun("obj[0] = new String(1002);");
  CHECK(security_check_with_gc_called);

  security_check_with_gc_called = false;
  CHECK(CompileRun("obj[0]")
            ->ToString(context)
            .ToLocalChecked()
            ->Equals(context, v8_str("1002"))
            .FromJust());
  CHECK(security_check_with_gc_called);
}


THREADED_TEST(CrossDomainDelete) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  CHECK(
      env1->Global()->Set(env1.local(), v8_str("prop"), v8_num(3)).FromJust());
  CHECK(env2->Global()->Set(env2, v8_str("env1"), env1->Global()).FromJust());

  // Change env2 to a different domain and delete env1.prop.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result =
        CompileRun("delete env1.prop");
    CHECK(result.IsEmpty());
  }

  // Check that env1.prop still exists.
  Local<Value> v =
      env1->Global()->Get(env1.local(), v8_str("prop")).ToLocalChecked();
  CHECK(v->IsNumber());
  CHECK_EQ(3, v->Int32Value(env1.local()).FromJust());
}


THREADED_TEST(CrossDomainPropertyIsEnumerable) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  CHECK(
      env1->Global()->Set(env1.local(), v8_str("prop"), v8_num(3)).FromJust());
  CHECK(env2->Global()->Set(env2, v8_str("env1"), env1->Global()).FromJust());

  // env1.prop is enumerable in env2.
  Local<String> test = v8_str("propertyIsEnumerable.call(env1, 'prop')");
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(test);
    CHECK(result->IsTrue());
  }

  // Change env2 to a different domain and test again.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(test);
    CHECK(result.IsEmpty());
  }
}


THREADED_TEST(CrossDomainFor) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  CHECK(
      env1->Global()->Set(env1.local(), v8_str("prop"), v8_num(3)).FromJust());
  CHECK(env2->Global()->Set(env2, v8_str("env1"), env1->Global()).FromJust());

  // Change env2 to a different domain and set env1's global object
  // as the __proto__ of an object in env2 and enumerate properties
  // in for-in. It shouldn't enumerate properties on env1's global
  // object. It shouldn't throw either, just silently ignore them.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(
        "(function() {"
        "  try {"
        "    for (var p in env1) {"
        "      if (p == 'prop') return false;"
        "    }"
        "    return true;"
        "  } catch (e) {"
        "    return false;"
        "  }"
        "})()");
    CHECK(result->IsTrue());
  }
}


THREADED_TEST(CrossDomainForInOnPrototype) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  CHECK(
      env1->Global()->Set(env1.local(), v8_str("prop"), v8_num(3)).FromJust());
  CHECK(env2->Global()->Set(env2, v8_str("env1"), env1->Global()).FromJust());

  // Change env2 to a different domain and set env1's global object
  // as the __proto__ of an object in env2 and enumerate properties
  // in for-in. It shouldn't enumerate properties on env1's global
  // object.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(
        "(function() {"
        "  var obj = { '__proto__': env1 };"
        "  try {"
        "    for (var p in obj) {"
        "      if (p == 'prop') return false;"
        "    }"
        "    return true;"
        "  } catch (e) {"
        "    return false;"
        "  }"
        "})()");
    CHECK(result->IsTrue());
  }
}


TEST(ContextDetachGlobal) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());


  Local<Value> foo = v8_str("foo");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  // Enter env2
  env2->Enter();

  // Create a function in env2 and add a reference to it in env1.
  Local<v8::Object> global2 = env2->Global();
  CHECK(global2->Set(env2, v8_str("prop"),
                     v8::Integer::New(env2->GetIsolate(), 1))
            .FromJust());
  CompileRun("function getProp() {return prop;}");

  CHECK(env1->Global()
            ->Set(env1.local(), v8_str("getProp"),
                  global2->Get(env2, v8_str("getProp")).ToLocalChecked())
            .FromJust());

  // Detach env2's global, and reuse the global object of env2
  env2->Exit();
  env2->DetachGlobal();

  v8::Local<Context> env3 = Context::New(
      env1->GetIsolate(), nullptr, v8::Local<v8::ObjectTemplate>(), global2);
  env3->SetSecurityToken(v8_str("bar"));

  env3->Enter();
  Local<v8::Object> global3 = env3->Global();
  CHECK(global2->Equals(env3, global3).FromJust());
  CHECK(global3->Get(env3, v8_str("prop")).ToLocalChecked()->IsUndefined());
  CHECK(global3->Get(env3, v8_str("getProp")).ToLocalChecked()->IsUndefined());
  CHECK(global3->Set(env3, v8_str("prop"),
                     v8::Integer::New(env3->GetIsolate(), -1))
            .FromJust());
  CHECK(global3->Set(env3, v8_str("prop2"),
                     v8::Integer::New(env3->GetIsolate(), 2))
            .FromJust());
  env3->Exit();

  // Call getProp in env1, and it should return the value 1
  {
    Local<v8::Object> global1 = env1->Global();
    Local<Value> get_prop =
        global1->Get(env1.local(), v8_str("getProp")).ToLocalChecked();
    CHECK(get_prop->IsFunction());
    v8::TryCatch try_catch(env1->GetIsolate());
    Local<Value> r = Function::Cast(*get_prop)
                         ->Call(env1.local(), global1, 0, nullptr)
                         .ToLocalChecked();
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(1, r->Int32Value(env1.local()).FromJust());
  }

  // Check that env3 is not accessible from env1
  {
    v8::MaybeLocal<Value> r = global3->Get(env1.local(), v8_str("prop2"));
    CHECK(r.IsEmpty());
  }
}


TEST(DetachGlobal) {
  LocalContext env1;
  v8::HandleScope scope(env1->GetIsolate());

  // Create second environment.
  v8::Local<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");

  // Set same security token for env1 and env2.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  // Create a property on the global object in env2.
  {
    v8::Context::Scope scope(env2);
    CHECK(env2->Global()
              ->Set(env2, v8_str("p"), v8::Integer::New(env2->GetIsolate(), 42))
              .FromJust());
  }

  // Create a reference to env2 global from env1 global.
  CHECK(env1->Global()
            ->Set(env1.local(), v8_str("other"), env2->Global())
            .FromJust());

  // Check that we have access to other.p in env2 from env1.
  Local<Value> result = CompileRun("other.p");
  CHECK(result->IsInt32());
  CHECK_EQ(42, result->Int32Value(env1.local()).FromJust());

  // Hold on to global from env2 and detach global from env2.
  Local<v8::Object> global2 = env2->Global();
  env2->DetachGlobal();

  // Check that the global has been detached. No other.p property can
  // be found.
  result = CompileRun("other.p");
  CHECK(result.IsEmpty());

  // Reuse global2 for env3.
  v8::Local<Context> env3 = Context::New(
      env1->GetIsolate(), nullptr, v8::Local<v8::ObjectTemplate>(), global2);
  CHECK(global2->Equals(env1.local(), env3->Global()).FromJust());

  // Start by using the same security token for env3 as for env1 and env2.
  env3->SetSecurityToken(foo);

  // Create a property on the global object in env3.
  {
    v8::Context::Scope scope(env3);
    CHECK(env3->Global()
              ->Set(env3, v8_str("p"), v8::Integer::New(env3->GetIsolate(), 24))
              .FromJust());
  }

  // Check that other.p is now the property in env3 and that we have access.
  result = CompileRun("other.p");
  CHECK(result->IsInt32());
  CHECK_EQ(24, result->Int32Value(env3).FromJust());
}


void GetThisX(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  info.GetReturnValue().Set(
      context->Global()->Get(context, v8_str("x")).ToLocalChecked());
}


TEST(DetachedAccesses) {
  LocalContext env1;
  v8::HandleScope scope(env1->GetIsolate());

  // Create second environment.
  Local<ObjectTemplate> inner_global_template =
      FunctionTemplate::New(env1->GetIsolate())->InstanceTemplate();
  inner_global_template ->SetAccessorProperty(
      v8_str("this_x"), FunctionTemplate::New(env1->GetIsolate(), GetThisX));
  v8::Local<Context> env2 =
      Context::New(env1->GetIsolate(), nullptr, inner_global_template);

  Local<Value> foo = v8_str("foo");

  // Set same security token for env1 and env2.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  CHECK(env1->Global()
            ->Set(env1.local(), v8_str("x"), v8_str("env1_x"))
            .FromJust());

  {
    v8::Context::Scope scope(env2);
    CHECK(env2->Global()->Set(env2, v8_str("x"), v8_str("env2_x")).FromJust());
    CompileRun(
        "function bound_x() { return x; }"
        "function get_x()   { return this.x; }"
        "function get_x_w() { return (function() {return this.x;})(); }");
    CHECK(env1->Global()
              ->Set(env1.local(), v8_str("bound_x"), CompileRun("bound_x"))
              .FromJust());
    CHECK(env1->Global()
              ->Set(env1.local(), v8_str("get_x"), CompileRun("get_x"))
              .FromJust());
    CHECK(env1->Global()
              ->Set(env1.local(), v8_str("get_x_w"), CompileRun("get_x_w"))
              .FromJust());
    env1->Global()
        ->Set(env1.local(), v8_str("this_x"),
              CompileRun("Object.getOwnPropertyDescriptor(this, 'this_x').get"))
        .FromJust();
  }

  Local<Object> env2_global = env2->Global();
  env2->DetachGlobal();

  Local<Value> result;
  result = CompileRun("bound_x()");
  CHECK(v8_str("env2_x")->Equals(env1.local(), result).FromJust());
  result = CompileRun("get_x()");
  CHECK(result.IsEmpty());
  result = CompileRun("get_x_w()");
  CHECK(result.IsEmpty());
  result = CompileRun("this_x()");
  CHECK(v8_str("env2_x")->Equals(env1.local(), result).FromJust());

  // Reattach env2's proxy
  env2 = Context::New(env1->GetIsolate(), nullptr,
                      v8::Local<v8::ObjectTemplate>(), env2_global);
  env2->SetSecurityToken(foo);
  {
    v8::Context::Scope scope(env2);
    CHECK(env2->Global()->Set(env2, v8_str("x"), v8_str("env3_x")).FromJust());
    CHECK(env2->Global()->Set(env2, v8_str("env1"), env1->Global()).FromJust());
    result = CompileRun(
        "results = [];"
        "for (var i = 0; i < 4; i++ ) {"
        "  results.push(env1.bound_x());"
        "  results.push(env1.get_x());"
        "  results.push(env1.get_x_w());"
        "  results.push(env1.this_x());"
        "}"
        "results");
    Local<v8::Array> results = Local<v8::Array>::Cast(result);
    CHECK_EQ(16u, results->Length());
    for (int i = 0; i < 16; i += 4) {
      CHECK(v8_str("env2_x")
                ->Equals(env2, results->Get(env2, i + 0).ToLocalChecked())
                .FromJust());
      CHECK(v8_str("env1_x")
                ->Equals(env2, results->Get(env2, i + 1).ToLocalChecked())
                .FromJust());
      CHECK(v8_str("env3_x")
                ->Equals(env2, results->Get(env2, i + 2).ToLocalChecked())
                .FromJust());
      CHECK(v8_str("env2_x")
                ->Equals(env2, results->Get(env2, i + 3).ToLocalChecked())
                .FromJust());
    }
  }

  result = CompileRun(
      "results = [];"
      "for (var i = 0; i < 4; i++ ) {"
      "  results.push(bound_x());"
      "  results.push(get_x());"
      "  results.push(get_x_w());"
      "  results.push(this_x());"
      "}"
      "results");
  Local<v8::Array> results = Local<v8::Array>::Cast(result);
  CHECK_EQ(16u, results->Length());
  for (int i = 0; i < 16; i += 4) {
    CHECK(v8_str("env2_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 0).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env3_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 1).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env3_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 2).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env2_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 3).ToLocalChecked())
              .FromJust());
  }

  result = CompileRun(
      "results = [];"
      "for (var i = 0; i < 4; i++ ) {"
      "  results.push(this.bound_x());"
      "  results.push(this.get_x());"
      "  results.push(this.get_x_w());"
      "  results.push(this.this_x());"
      "}"
      "results");
  results = Local<v8::Array>::Cast(result);
  CHECK_EQ(16u, results->Length());
  for (int i = 0; i < 16; i += 4) {
    CHECK(v8_str("env2_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 0).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env1_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 1).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env3_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 2).ToLocalChecked())
              .FromJust());
    CHECK(v8_str("env2_x")
              ->Equals(env1.local(),
                       results->Get(env1.local(), i + 3).ToLocalChecked())
              .FromJust());
  }
}


static bool allowed_access = false;
static bool AccessBlocker(Local<v8::Context> accessing_context,
                          Local<v8::Object> accessed_object,
                          Local<v8::Value> data) {
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  return context->Global()->Equals(context, accessed_object).FromJust() ||
         allowed_access;
}


static int g_echo_value = -1;


static void EchoGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(g_echo_value));
}


static void EchoSetter(Local<String> name, Local<Value> value,
                       const v8::PropertyCallbackInfo<void>& args) {
  if (value->IsNumber())
    g_echo_value =
        value->Int32Value(args.GetIsolate()->GetCurrentContext()).FromJust();
}


static void UnreachableGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  UNREACHABLE();  // This function should not be called..
}


static void UnreachableSetter(Local<String>,
                              Local<Value>,
                              const v8::PropertyCallbackInfo<void>&) {
  UNREACHABLE();  // This function should not be called.
}


static void UnreachableFunction(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  UNREACHABLE();  // This function should not be called..
}


TEST(AccessControl) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);

  global_template->SetAccessCheckCallback(AccessBlocker);

  // Add an accessor accessible by cross-domain JS code.
  global_template->SetAccessor(
      v8_str("accessible_prop"), EchoGetter, EchoSetter, v8::Local<Value>(),
      v8::AccessControl(v8::ALL_CAN_READ | v8::ALL_CAN_WRITE));


  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("blocked_prop"), UnreachableGetter,
                               UnreachableSetter, v8::Local<Value>(),
                               v8::DEFAULT);

  global_template->SetAccessorProperty(
      v8_str("blocked_js_prop"),
      v8::FunctionTemplate::New(isolate, UnreachableFunction),
      v8::FunctionTemplate::New(isolate, UnreachableFunction),
      v8::None,
      v8::DEFAULT);

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, nullptr, global_template);
  context0->Enter();

  v8::Local<v8::Object> global0 = context0->Global();

  // Define a property with JS getter and setter.
  CompileRun(
      "function getter() { return 'getter'; };\n"
      "function setter() { return 'setter'; }\n"
      "Object.defineProperty(this, 'js_accessor_p', {get:getter, set:setter})");

  Local<Value> getter =
      global0->Get(context0, v8_str("getter")).ToLocalChecked();
  Local<Value> setter =
      global0->Get(context0, v8_str("setter")).ToLocalChecked();

  // And define normal element.
  CHECK(global0->Set(context0, 239, v8_str("239")).FromJust());

  // Define an element with JS getter and setter.
  CompileRun(
      "function el_getter() { return 'el_getter'; };\n"
      "function el_setter() { return 'el_setter'; };\n"
      "Object.defineProperty(this, '42', {get: el_getter, set: el_setter});");

  Local<Value> el_getter =
      global0->Get(context0, v8_str("el_getter")).ToLocalChecked();
  Local<Value> el_setter =
      global0->Get(context0, v8_str("el_setter")).ToLocalChecked();

  v8::HandleScope scope1(isolate);

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1, v8_str("other"), global0).FromJust());

  // Access blocked property.
  CompileRun("other.blocked_prop = 1");

  CHECK(CompileRun("other.blocked_prop").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'blocked_prop')")
            .IsEmpty());
  CHECK(
      CompileRun("propertyIsEnumerable.call(other, 'blocked_prop')").IsEmpty());

  // Access blocked element.
  CHECK(CompileRun("other[239] = 1").IsEmpty());

  CHECK(CompileRun("other[239]").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, '239')").IsEmpty());
  CHECK(CompileRun("propertyIsEnumerable.call(other, '239')").IsEmpty());

  allowed_access = true;
  // Now we can enumerate the property.
  ExpectTrue("propertyIsEnumerable.call(other, '239')");
  allowed_access = false;

  // Access a property with JS accessor.
  CHECK(CompileRun("other.js_accessor_p = 2").IsEmpty());

  CHECK(CompileRun("other.js_accessor_p").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'js_accessor_p')")
            .IsEmpty());

  allowed_access = true;

  ExpectString("other.js_accessor_p", "getter");
  ExpectObject(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').get", getter);
  ExpectObject(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').set", setter);
  ExpectUndefined(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').value");

  allowed_access = false;

  // Access an element with JS accessor.
  CHECK(CompileRun("other[42] = 2").IsEmpty());

  CHECK(CompileRun("other[42]").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, '42')").IsEmpty());

  allowed_access = true;

  ExpectString("other[42]", "el_getter");
  ExpectObject("Object.getOwnPropertyDescriptor(other, '42').get", el_getter);
  ExpectObject("Object.getOwnPropertyDescriptor(other, '42').set", el_setter);
  ExpectUndefined("Object.getOwnPropertyDescriptor(other, '42').value");

  allowed_access = false;

  v8::Local<Value> value;

  // Access accessible property
  value = CompileRun("other.accessible_prop = 3");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value(context1).FromJust());
  CHECK_EQ(3, g_echo_value);

  value = CompileRun("other.accessible_prop");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value(context1).FromJust());

  value = CompileRun(
      "Object.getOwnPropertyDescriptor(other, 'accessible_prop').value");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value(context1).FromJust());

  value = CompileRun("propertyIsEnumerable.call(other, 'accessible_prop')");
  CHECK(value->IsTrue());

  // Enumeration doesn't enumerate accessors from inaccessible objects in
  // the prototype chain even if the accessors are in themselves accessible.
  // Enumeration doesn't throw, it silently ignores what it can't access.
  value = CompileRun(
      "(function() {"
      "  var obj = { '__proto__': other };"
      "  try {"
      "    for (var p in obj) {"
      "      if (p == 'accessible_prop' ||"
      "          p == 'blocked_js_prop' ||"
      "          p == 'blocked_js_prop') {"
      "        return false;"
      "      }"
      "    }"
      "    return true;"
      "  } catch (e) {"
      "    return false;"
      "  }"
      "})()");
  CHECK(value->IsTrue());

  // Test that preventExtensions fails on a non-accessible object even if that
  // object is already non-extensible.
  CHECK(global1->Set(context1, v8_str("checked_object"),
                     global_template->NewInstance(context1).ToLocalChecked())
            .FromJust());
  allowed_access = true;
  CompileRun("Object.preventExtensions(checked_object)");
  ExpectFalse("Object.isExtensible(checked_object)");
  allowed_access = false;
  CHECK(CompileRun("Object.preventExtensions(checked_object)").IsEmpty());

  context1->Exit();
  context0->Exit();
}


TEST(AccessControlES5) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);

  global_template->SetAccessCheckCallback(AccessBlocker);

  // Add accessible accessor.
  global_template->SetAccessor(
      v8_str("accessible_prop"), EchoGetter, EchoSetter, v8::Local<Value>(),
      v8::AccessControl(v8::ALL_CAN_READ | v8::ALL_CAN_WRITE));


  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("blocked_prop"), UnreachableGetter,
                               UnreachableSetter, v8::Local<Value>(),
                               v8::DEFAULT);

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, nullptr, global_template);
  context0->Enter();

  v8::Local<v8::Object> global0 = context0->Global();

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();
  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1, v8_str("other"), global0).FromJust());

  // Regression test for issue 1154.
  CHECK(CompileRun("Object.keys(other).length == 1")->BooleanValue(isolate));
  CHECK(CompileRun("Object.keys(other)[0] == 'accessible_prop'")
            ->BooleanValue(isolate));
  CHECK(CompileRun("other.blocked_prop").IsEmpty());

  // Regression test for issue 1027.
  CompileRun("Object.defineProperty(\n"
             "  other, 'blocked_prop', {configurable: false})");
  CHECK(CompileRun("other.blocked_prop").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'blocked_prop')")
            .IsEmpty());

  // Regression test for issue 1171.
  ExpectTrue("Object.isExtensible(other)");
  CompileRun("Object.preventExtensions(other)");
  ExpectTrue("Object.isExtensible(other)");

  // Object.seal and Object.freeze.
  CompileRun("Object.freeze(other)");
  ExpectTrue("Object.isExtensible(other)");

  CompileRun("Object.seal(other)");
  ExpectTrue("Object.isExtensible(other)");

  // Regression test for issue 1250.
  // Make sure that we can set the accessible accessors value using normal
  // assignment.
  CompileRun("other.accessible_prop = 42");
  CHECK_EQ(42, g_echo_value);

  // [[DefineOwnProperty]] always throws for access-checked objects.
  CHECK(
      CompileRun("Object.defineProperty(other, 'accessible_prop', {value: 43})")
          .IsEmpty());
  CHECK(CompileRun("other.accessible_prop == 42")->IsTrue());
  CHECK_EQ(42, g_echo_value);  // Make sure we didn't call the setter.
}

static bool AccessAlwaysBlocked(Local<v8::Context> accessing_context,
                                Local<v8::Object> global,
                                Local<v8::Value> data) {
  i::PrintF("Access blocked.\n");
  return false;
}

static bool AccessAlwaysAllowed(Local<v8::Context> accessing_context,
                                Local<v8::Object> global,
                                Local<v8::Value> data) {
  i::PrintF("Access allowed.\n");
  return true;
}

THREADED_TEST(AccessControlGetOwnPropertyNames) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> obj_template = v8::ObjectTemplate::New(isolate);

  obj_template->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  obj_template->SetAccessCheckCallback(AccessAlwaysBlocked);

  // Add an accessor accessible by cross-domain JS code.
  obj_template->SetAccessor(
      v8_str("accessible_prop"), EchoGetter, EchoSetter, v8::Local<Value>(),
      v8::AccessControl(v8::ALL_CAN_READ | v8::ALL_CAN_WRITE));

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, nullptr, obj_template);
  context0->Enter();

  v8::Local<v8::Object> global0 = context0->Global();

  v8::HandleScope scope1(CcTest::isolate());

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1, v8_str("other"), global0).FromJust());
  CHECK(global1->Set(context1, v8_str("object"),
                     obj_template->NewInstance(context1).ToLocalChecked())
            .FromJust());

  v8::Local<Value> value;

  // Attempt to get the property names of the other global object and
  // of an object that requires access checks.  Accessing the other
  // global object should be blocked by access checks on the global
  // proxy object.  Accessing the object that requires access checks
  // is blocked by the access checks on the object itself.
  value = CompileRun(
      "var names = Object.getOwnPropertyNames(other);"
      "names.length == 1 && names[0] == 'accessible_prop';");
  CHECK(value->BooleanValue(isolate));

  value = CompileRun(
      "var names = Object.getOwnPropertyNames(object);"
      "names.length == 1 && names[0] == 'accessible_prop';");
  CHECK(value->BooleanValue(isolate));

  context1->Exit();
  context0->Exit();
}


TEST(Regress470113) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> obj_template = v8::ObjectTemplate::New(isolate);
  obj_template->SetAccessCheckCallback(AccessAlwaysBlocked);
  LocalContext env;
  CHECK(env->Global()
            ->Set(env.local(), v8_str("prohibited"),
                  obj_template->NewInstance(env.local()).ToLocalChecked())
            .FromJust());

  {
    v8::TryCatch try_catch(isolate);
    CompileRun(
        "'use strict';\n"
        "class C extends Object {\n"
        "   m() { super.powned = 'Powned!'; }\n"
        "}\n"
        "let c = new C();\n"
        "c.m.call(prohibited)");

    CHECK(try_catch.HasCaught());
  }
}


static void ConstTenGetter(Local<String> name,
                           const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(10));
}


THREADED_TEST(CrossDomainAccessors) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::FunctionTemplate> func_template =
      v8::FunctionTemplate::New(isolate);

  v8::Local<v8::ObjectTemplate> global_template =
      func_template->InstanceTemplate();

  v8::Local<v8::ObjectTemplate> proto_template =
      func_template->PrototypeTemplate();

  // Add an accessor to proto that's accessible by cross-domain JS code.
  proto_template->SetAccessor(v8_str("accessible"), ConstTenGetter, nullptr,
                              v8::Local<Value>(), v8::ALL_CAN_READ);

  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("unreachable"), UnreachableGetter,
                               nullptr, v8::Local<Value>(), v8::DEFAULT);

  v8::Local<Context> context0 = Context::New(isolate, nullptr, global_template);
  context0->Enter();

  Local<v8::Object> global = context0->Global();
  // Add a normal property that shadows 'accessible'
  CHECK(global->Set(context0, v8_str("accessible"), v8_num(11)).FromJust());

  // Enter a new context.
  v8::HandleScope scope1(CcTest::isolate());
  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1, v8_str("other"), global).FromJust());

  // Should return 10, instead of 11
  v8::Local<Value> value =
      v8_compile("other.accessible")->Run(context1).ToLocalChecked();
  CHECK(value->IsNumber());
  CHECK_EQ(10, value->Int32Value(context1).FromJust());

  v8::MaybeLocal<v8::Value> maybe_value =
      v8_compile("other.unreachable")->Run(context1);
  CHECK(maybe_value.IsEmpty());

  context1->Exit();
  context0->Exit();
}


static int access_count = 0;

static bool AccessCounter(Local<v8::Context> accessing_context,
                          Local<v8::Object> accessed_object,
                          Local<v8::Value> data) {
  access_count++;
  return true;
}


// This one is too easily disturbed by other tests.
TEST(AccessControlIC) {
  access_count = 0;

  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  // Create an environment.
  v8::Local<Context> context0 = Context::New(isolate);
  context0->Enter();

  // Create an object that requires access-check functions to be
  // called for cross-domain access.
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(AccessCounter);
  Local<v8::Object> object =
      object_template->NewInstance(context0).ToLocalChecked();

  v8::HandleScope scope1(isolate);

  // Create another environment.
  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  // Make easy access to the object from the other environment.
  v8::Local<v8::Object> global1 = context1->Global();
  CHECK(global1->Set(context1, v8_str("obj"), object).FromJust());

  v8::Local<Value> value;

  // Check that the named access-control function is called every time.
  CompileRun("function testProp(obj) {"
             "  for (var i = 0; i < 10; i++) obj.prop = 1;"
             "  for (var j = 0; j < 10; j++) obj.prop;"
             "  return obj.prop"
             "}");
  value = CompileRun("testProp(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(21, access_count);

  // Check that the named access-control function is called every time.
  CompileRun("var p = 'prop';"
             "function testKeyed(obj) {"
             "  for (var i = 0; i < 10; i++) obj[p] = 1;"
             "  for (var j = 0; j < 10; j++) obj[p];"
             "  return obj[p];"
             "}");
  // Use obj which requires access checks.  No inline caching is used
  // in that case.
  value = CompileRun("testKeyed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(42, access_count);
  // Force the inline caches into generic state and try again.
  CompileRun("testKeyed({ a: 0 })");
  CompileRun("testKeyed({ b: 0 })");
  value = CompileRun("testKeyed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(63, access_count);

  // Check that the indexed access-control function is called every time.
  access_count = 0;

  CompileRun("function testIndexed(obj) {"
             "  for (var i = 0; i < 10; i++) obj[0] = 1;"
             "  for (var j = 0; j < 10; j++) obj[0];"
             "  return obj[0]"
             "}");
  value = CompileRun("testIndexed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(21, access_count);
  // Force the inline caches into generic state.
  CompileRun("testIndexed(new Array(1))");
  // Test that the indexed access check is called.
  value = CompileRun("testIndexed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(42, access_count);

  access_count = 0;
  // Check that the named access check is called when invoking
  // functions on an object that requires access checks.
  CompileRun("obj.f = function() {}");
  CompileRun("function testCallNormal(obj) {"
             "  for (var i = 0; i < 10; i++) obj.f();"
             "}");
  CompileRun("testCallNormal(obj)");
  printf("%i\n", access_count);
  CHECK_EQ(11, access_count);

  // Force obj into slow case.
  value = CompileRun("delete obj.prop");
  CHECK(value->BooleanValue(isolate));
  // Force inline caches into dictionary probing mode.
  CompileRun("var o = { x: 0 }; delete o.x; testProp(o);");
  // Test that the named access check is called.
  value = CompileRun("testProp(obj);");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value(context1).FromJust());
  CHECK_EQ(33, access_count);

  // Force the call inline cache into dictionary probing mode.
  CompileRun("o.f = function() {}; testCallNormal(o)");
  // Test that the named access check is still called for each
  // invocation of the function.
  value = CompileRun("testCallNormal(obj)");
  CHECK_EQ(43, access_count);

  context1->Exit();
  context0->Exit();
}


THREADED_TEST(Version) { v8::V8::GetVersion(); }


static void InstanceFunctionCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(12));
}


THREADED_TEST(InstanceProperties) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> instance = t->InstanceTemplate();

  instance->Set(v8_str("x"), v8_num(42));
  instance->Set(v8_str("f"),
                v8::FunctionTemplate::New(isolate, InstanceFunctionCallback));

  Local<Value> o = t->GetFunction(context.local())
                       .ToLocalChecked()
                       ->NewInstance(context.local())
                       .ToLocalChecked();

  CHECK(context->Global()->Set(context.local(), v8_str("i"), o).FromJust());
  Local<Value> value = CompileRun("i.x");
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());

  value = CompileRun("i.f()");
  CHECK_EQ(12, value->Int32Value(context.local()).FromJust());
}


static void GlobalObjectInstancePropertiesGet(
    Local<Name> key, const v8::PropertyCallbackInfo<v8::Value>&) {
  ApiTestFuzzer::Fuzz();
}

static int script_execution_count = 0;
static void ScriptExecutionCallback(v8::Isolate* isolate,
                                    Local<Context> context) {
  script_execution_count++;
}

THREADED_TEST(ContextScriptExecutionCallback) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext context;

  {
    v8::TryCatch try_catch(isolate);
    script_execution_count = 0;
    ExpectTrue("1 + 1 == 2");
    CHECK_EQ(0, script_execution_count);
    CHECK(!try_catch.HasCaught());
  }

  context->SetAbortScriptExecution(ScriptExecutionCallback);

  {  // Function binding does not trigger callback.
    v8::Local<v8::FunctionTemplate> function_template =
        v8::FunctionTemplate::New(isolate, DummyCallHandler);
    v8::Local<v8::Function> function =
        function_template->GetFunction(context.local()).ToLocalChecked();

    v8::TryCatch try_catch(isolate);
    script_execution_count = 0;

    CHECK_EQ(13.4,
             function->Call(context.local(), v8::Undefined(isolate), 0, nullptr)
                 .ToLocalChecked()
                 ->NumberValue(context.local())
                 .FromJust());
    CHECK_EQ(0, script_execution_count);
    CHECK(!try_catch.HasCaught());
  }

  {  // Script execution triggers callback.
    v8::TryCatch try_catch(isolate);
    script_execution_count = 0;
    CHECK(CompileRun(context.local(), "2 + 2 == 4").IsEmpty());
    CHECK_EQ(1, script_execution_count);
    CHECK(try_catch.HasCaught());
  }

  context->SetAbortScriptExecution(nullptr);

  {  // Script execution no longer triggers callback.
    v8::TryCatch try_catch(isolate);
    script_execution_count = 0;
    ExpectTrue("2 + 2 == 4");
    CHECK_EQ(0, script_execution_count);
    CHECK(!try_catch.HasCaught());
  }
}

THREADED_TEST(GlobalObjectInstanceProperties) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<Value> global_object;

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(GlobalObjectInstancePropertiesGet));
  Local<ObjectTemplate> instance_template = t->InstanceTemplate();
  instance_template->Set(v8_str("x"), v8_num(42));
  instance_template->Set(v8_str("f"),
                         v8::FunctionTemplate::New(isolate,
                                                   InstanceFunctionCallback));

  // The script to check how TurboFan compiles missing global function
  // invocations.  function g is not defined and should throw on call.
  const char* script =
      "function wrapper(call) {"
      "  var x = 0, y = 1;"
      "  for (var i = 0; i < 1000; i++) {"
      "    x += i * 100;"
      "    y += i * 100;"
      "  }"
      "  if (call) g();"
      "}"
      "for (var i = 0; i < 17; i++) wrapper(false);"
      "var thrown = 0;"
      "try { wrapper(true); } catch (e) { thrown = 1; };"
      "thrown";

  {
    LocalContext env(nullptr, instance_template);
    // Hold on to the global object so it can be used again in another
    // environment initialization.
    global_object = env->Global();

    Local<Value> value = CompileRun("x");
    CHECK_EQ(42, value->Int32Value(env.local()).FromJust());
    value = CompileRun("f()");
    CHECK_EQ(12, value->Int32Value(env.local()).FromJust());
    value = CompileRun(script);
    CHECK_EQ(1, value->Int32Value(env.local()).FromJust());
  }

  {
    // Create new environment reusing the global object.
    LocalContext env(nullptr, instance_template, global_object);
    Local<Value> value = CompileRun("x");
    CHECK_EQ(42, value->Int32Value(env.local()).FromJust());
    value = CompileRun("f()");
    CHECK_EQ(12, value->Int32Value(env.local()).FromJust());
    value = CompileRun(script);
    CHECK_EQ(1, value->Int32Value(env.local()).FromJust());
  }
}

THREADED_TEST(ObjectGetOwnPropertyNames) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Object> value = v8::Local<v8::Object>::Cast(
      v8::StringObject::New(CcTest::isolate(), v8_str("test")));
  v8::Local<v8::Array> properties;

  CHECK(value
            ->GetOwnPropertyNames(context.local(),
                                  static_cast<v8::PropertyFilter>(
                                      v8::PropertyFilter::ALL_PROPERTIES |
                                      v8::PropertyFilter::SKIP_SYMBOLS),
                                  v8::KeyConversionMode::kKeepNumbers)
            .ToLocal(&properties));
  CHECK_EQ(5u, properties->Length());
  v8::Local<v8::Value> property;
  CHECK(properties->Get(context.local(), 4).ToLocal(&property) &&
        property->IsString());
  CHECK(property.As<v8::String>()
            ->Equals(context.local(), v8_str("length"))
            .FromMaybe(false));
  for (int i = 0; i < 4; ++i) {
    v8::Local<v8::Value> property;
    CHECK(properties->Get(context.local(), i).ToLocal(&property) &&
          property->IsInt32());
    CHECK_EQ(property.As<v8::Int32>()->Value(), i);
  }

  CHECK(value
            ->GetOwnPropertyNames(context.local(),
                                  v8::PropertyFilter::ONLY_ENUMERABLE,
                                  v8::KeyConversionMode::kKeepNumbers)
            .ToLocal(&properties));
  v8::Local<v8::Array> number_properties;
  CHECK(value
            ->GetOwnPropertyNames(context.local(),
                                  v8::PropertyFilter::ONLY_ENUMERABLE,
                                  v8::KeyConversionMode::kConvertToString)
            .ToLocal(&number_properties));
  CHECK_EQ(4u, properties->Length());
  for (int i = 0; i < 4; ++i) {
    v8::Local<v8::Value> property_index;
    v8::Local<v8::Value> property_name;

    CHECK(number_properties->Get(context.local(), i).ToLocal(&property_name));
    CHECK(property_name->IsString());

    CHECK(properties->Get(context.local(), i).ToLocal(&property_index));
    CHECK(property_index->IsInt32());

    CHECK_EQ(property_index.As<v8::Int32>()->Value(), i);
    CHECK_EQ(property_name->ToNumber(context.local())
                 .ToLocalChecked()
                 .As<v8::Int32>()
                 ->Value(),
             i);
  }

  value = value->GetPrototype().As<v8::Object>();
  CHECK(value
            ->GetOwnPropertyNames(context.local(),
                                  static_cast<v8::PropertyFilter>(
                                      v8::PropertyFilter::ALL_PROPERTIES |
                                      v8::PropertyFilter::SKIP_SYMBOLS))
            .ToLocal(&properties));
  bool concat_found = false;
  bool starts_with_found = false;
  for (uint32_t i = 0; i < properties->Length(); ++i) {
    v8::Local<v8::Value> property;
    CHECK(properties->Get(context.local(), i).ToLocal(&property));
    if (!property->IsString()) continue;
    if (!concat_found)
      concat_found = property.As<v8::String>()
                         ->Equals(context.local(), v8_str("concat"))
                         .FromMaybe(false);
    if (!starts_with_found)
      starts_with_found = property.As<v8::String>()
                              ->Equals(context.local(), v8_str("startsWith"))
                              .FromMaybe(false);
  }
  CHECK(concat_found && starts_with_found);
}

THREADED_TEST(CallKnownGlobalReceiver) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<Value> global_object;

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> instance_template = t->InstanceTemplate();

  // The script to check that we leave global object not
  // global object proxy on stack when we deoptimize from inside
  // arguments evaluation.
  // To provoke error we need to both force deoptimization
  // from arguments evaluation and to force CallIC to take
  // CallIC_Miss code path that can't cope with global proxy.
  const char* script =
      "function bar(x, y) { try { } finally { } }"
      "function baz(x) { try { } finally { } }"
      "function bom(x) { try { } finally { } }"
      "function foo(x) { bar([x], bom(2)); }"
      "for (var i = 0; i < 10000; i++) foo(1);"
      "foo";

  Local<Value> foo;
  {
    LocalContext env(nullptr, instance_template);
    // Hold on to the global object so it can be used again in another
    // environment initialization.
    global_object = env->Global();
    foo = CompileRun(script);
  }

  {
    // Create new environment reusing the global object.
    LocalContext env(nullptr, instance_template, global_object);
    CHECK(env->Global()->Set(env.local(), v8_str("foo"), foo).FromJust());
    CompileRun("foo()");
  }
}


static void ShadowFunctionCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(42));
}


static int shadow_y;
static int shadow_y_setter_call_count;
static int shadow_y_getter_call_count;


static void ShadowYSetter(Local<String>,
                          Local<Value>,
                          const v8::PropertyCallbackInfo<void>&) {
  shadow_y_setter_call_count++;
  shadow_y = 42;
}


static void ShadowYGetter(Local<String> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  shadow_y_getter_call_count++;
  info.GetReturnValue().Set(v8_num(shadow_y));
}


static void ShadowIndexedGet(uint32_t index,
                             const v8::PropertyCallbackInfo<v8::Value>&) {
}


static void ShadowNamedGet(Local<Name> key,
                           const v8::PropertyCallbackInfo<v8::Value>&) {}

THREADED_TEST(ShadowObject) {
  shadow_y = shadow_y_setter_call_count = shadow_y_getter_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  LocalContext context(nullptr, global_template);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(ShadowNamedGet));
  t->InstanceTemplate()->SetHandler(
      v8::IndexedPropertyHandlerConfiguration(ShadowIndexedGet));
  Local<ObjectTemplate> proto = t->PrototypeTemplate();
  Local<ObjectTemplate> instance = t->InstanceTemplate();

  proto->Set(v8_str("f"),
             v8::FunctionTemplate::New(isolate,
                                       ShadowFunctionCallback,
                                       Local<Value>()));
  proto->Set(v8_str("x"), v8_num(12));

  instance->SetAccessor(v8_str("y"), ShadowYGetter, ShadowYSetter);

  Local<Value> o = t->GetFunction(context.local())
                       .ToLocalChecked()
                       ->NewInstance(context.local())
                       .ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("__proto__"), o)
            .FromJust());

  Local<Value> value =
      CompileRun("this.propertyIsEnumerable(0)");
  CHECK(value->IsBoolean());
  CHECK(!value->BooleanValue(isolate));

  value = CompileRun("x");
  CHECK_EQ(12, value->Int32Value(context.local()).FromJust());

  value = CompileRun("f()");
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());

  CompileRun("y = 43");
  CHECK_EQ(1, shadow_y_setter_call_count);
  value = CompileRun("y");
  CHECK_EQ(1, shadow_y_getter_call_count);
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());
}

THREADED_TEST(ShadowObjectAndDataProperty) {
  // Lite mode doesn't make use of feedback vectors, which is what we
  // want to ensure has the correct form.
  if (i::FLAG_lite_mode) return;
  // This test mimics the kind of shadow property the Chromium embedder
  // uses for undeclared globals. The IC subsystem has special handling
  // for this case, using a PREMONOMORPHIC state to delay entering
  // MONOMORPHIC state until enough information is available to support
  // efficient access and good feedback for optimization.
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  i::FLAG_allow_natives_syntax = true;

  Local<ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  LocalContext context(nullptr, global_template);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(ShadowNamedGet));

  Local<Value> o = t->GetFunction(context.local())
                       .ToLocalChecked()
                       ->NewInstance(context.local())
                       .ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("__proto__"), o)
            .FromJust());

  CompileRun(
      "function foo(x) { i = x; }"
      "%EnsureFeedbackVectorForFunction(foo);"
      "foo(0)");

  i::Handle<i::JSFunction> foo(i::Handle<i::JSFunction>::cast(
      v8::Utils::OpenHandle(*context->Global()
                                 ->Get(context.local(), v8_str("foo"))
                                 .ToLocalChecked())));
  CHECK(foo->has_feedback_vector());
  i::FeedbackSlot slot = i::FeedbackVector::ToSlot(0);
  i::FeedbackNexus nexus(foo->feedback_vector(), slot);
  CHECK_EQ(i::FeedbackSlotKind::kStoreGlobalSloppy, nexus.kind());
  CHECK_EQ(i::PREMONOMORPHIC, nexus.ic_state());
  CompileRun("foo(1)");
  CHECK_EQ(i::MONOMORPHIC, nexus.ic_state());
  // We go a bit further, checking that the form of monomorphism is
  // a PropertyCell in the vector. This is because we want to make sure
  // we didn't settle for a "poor man's monomorphism," such as a
  // slow_stub bailout which would mean a trip to the runtime on all
  // subsequent stores, and a lack of feedback for the optimizing
  // compiler downstream.
  i::HeapObject heap_object;
  CHECK(nexus.GetFeedback().GetHeapObject(&heap_object));
  CHECK(heap_object.IsPropertyCell());
}

THREADED_TEST(ShadowObjectAndDataPropertyTurbo) {
  // This test is the same as the previous one except that it triggers
  // optimization of {foo} after its first invocation.
  i::FLAG_allow_natives_syntax = true;

  if (i::FLAG_lite_mode) return;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  LocalContext context(nullptr, global_template);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(ShadowNamedGet));

  Local<Value> o = t->GetFunction(context.local())
                       .ToLocalChecked()
                       ->NewInstance(context.local())
                       .ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("__proto__"), o)
            .FromJust());

  CompileRun(
      "function foo(x) { i = x; };"
      "%PrepareFunctionForOptimization(foo);"
      "foo(0)");

  i::Handle<i::JSFunction> foo(i::Handle<i::JSFunction>::cast(
      v8::Utils::OpenHandle(*context->Global()
                                 ->Get(context.local(), v8_str("foo"))
                                 .ToLocalChecked())));
  CHECK(foo->has_feedback_vector());
  i::FeedbackSlot slot = i::FeedbackVector::ToSlot(0);
  i::FeedbackNexus nexus(foo->feedback_vector(), slot);
  CHECK_EQ(i::FeedbackSlotKind::kStoreGlobalSloppy, nexus.kind());
  CHECK_EQ(i::PREMONOMORPHIC, nexus.ic_state());
  CompileRun("%OptimizeFunctionOnNextCall(foo); foo(1)");
  CHECK_EQ(i::MONOMORPHIC, nexus.ic_state());
  i::HeapObject heap_object;
  CHECK(nexus.GetFeedback().GetHeapObject(&heap_object));
  CHECK(heap_object.IsPropertyCell());
}

THREADED_TEST(SetPrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t0 = v8::FunctionTemplate::New(isolate);
  t0->InstanceTemplate()->Set(v8_str("x"), v8_num(0));
  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->InstanceTemplate()->Set(v8_str("y"), v8_num(1));
  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->InstanceTemplate()->Set(v8_str("z"), v8_num(2));
  Local<v8::FunctionTemplate> t3 = v8::FunctionTemplate::New(isolate);
  t3->InstanceTemplate()->Set(v8_str("u"), v8_num(3));

  Local<v8::Object> o0 = t0->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o1 = t1->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o2 = t2->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o3 = t3->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();

  CHECK_EQ(0, o0->Get(context.local(), v8_str("x"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(o0->SetPrototype(context.local(), o1).FromJust());
  CHECK_EQ(0, o0->Get(context.local(), v8_str("x"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(1, o0->Get(context.local(), v8_str("y"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(o1->SetPrototype(context.local(), o2).FromJust());
  CHECK_EQ(0, o0->Get(context.local(), v8_str("x"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(1, o0->Get(context.local(), v8_str("y"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(2, o0->Get(context.local(), v8_str("z"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK(o2->SetPrototype(context.local(), o3).FromJust());
  CHECK_EQ(0, o0->Get(context.local(), v8_str("x"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(1, o0->Get(context.local(), v8_str("y"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(2, o0->Get(context.local(), v8_str("z"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(3, o0->Get(context.local(), v8_str("u"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());

  Local<Value> proto =
      o0->Get(context.local(), v8_str("__proto__")).ToLocalChecked();
  CHECK(proto->IsObject());
  CHECK(proto.As<v8::Object>()->Equals(context.local(), o1).FromJust());

  Local<Value> proto0 = o0->GetPrototype();
  CHECK(proto0->IsObject());
  CHECK(proto0.As<v8::Object>()->Equals(context.local(), o1).FromJust());

  Local<Value> proto1 = o1->GetPrototype();
  CHECK(proto1->IsObject());
  CHECK(proto1.As<v8::Object>()->Equals(context.local(), o2).FromJust());

  Local<Value> proto2 = o2->GetPrototype();
  CHECK(proto2->IsObject());
  CHECK(proto2.As<v8::Object>()->Equals(context.local(), o3).FromJust());
}


// Getting property names of an object with a prototype chain that
// triggers dictionary elements in GetOwnPropertyNames() shouldn't
// crash the runtime.
THREADED_TEST(Regress91517) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->InstanceTemplate()->Set(v8_str("foo"), v8_num(1));
  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->InstanceTemplate()->Set(v8_str("fuz1"), v8_num(2));
  t2->InstanceTemplate()->Set(v8_str("objects"),
                              v8::ObjectTemplate::New(isolate));
  t2->InstanceTemplate()->Set(v8_str("fuz2"), v8_num(2));
  Local<v8::FunctionTemplate> t3 = v8::FunctionTemplate::New(isolate);
  t3->InstanceTemplate()->Set(v8_str("boo"), v8_num(3));
  Local<v8::FunctionTemplate> t4 = v8::FunctionTemplate::New(isolate);
  t4->InstanceTemplate()->Set(v8_str("baz"), v8_num(4));

  // Force dictionary-based properties.
  i::ScopedVector<char> name_buf(1024);
  for (int i = 1; i <= 1000; i++) {
    i::SNPrintF(name_buf, "sdf%d", i);
    t2->InstanceTemplate()->Set(v8_str(name_buf.begin()), v8_num(2));
  }

  Local<v8::Object> o1 = t1->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o2 = t2->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o3 = t3->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o4 = t4->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();

  CHECK(o4->SetPrototype(context.local(), o3).FromJust());
  CHECK(o3->SetPrototype(context.local(), o2).FromJust());
  CHECK(o2->SetPrototype(context.local(), o1).FromJust());

  // Call the runtime version of GetOwnPropertyNames() on the natively
  // created object through JavaScript.
  CHECK(context->Global()->Set(context.local(), v8_str("obj"), o4).FromJust());
  // PROPERTY_FILTER_NONE = 0
  CompileRun("var names = %GetOwnPropertyKeys(obj, 0);");

  ExpectInt32("names.length", 1);
  ExpectTrue("names.indexOf(\"baz\") >= 0");
  ExpectFalse("names.indexOf(\"boo\") >= 0");
  ExpectFalse("names.indexOf(\"foo\") >= 0");
  ExpectFalse("names.indexOf(\"fuz1\") >= 0");
  ExpectFalse("names.indexOf(\"objects\") >= 0");
  ExpectFalse("names.indexOf(\"fuz2\") >= 0");
  ExpectTrue("names[1005] == undefined");
}


THREADED_TEST(FunctionReadOnlyPrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->PrototypeTemplate()->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  t1->ReadOnlyPrototype();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("func1"),
                  t1->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  // Configured value of ReadOnly flag.
  CHECK(
      CompileRun(
          "(function() {"
          "  descriptor = Object.getOwnPropertyDescriptor(func1, 'prototype');"
          "  return (descriptor['writable'] == false);"
          "})()")
          ->BooleanValue(isolate));
  CHECK_EQ(
      42,
      CompileRun("func1.prototype.x")->Int32Value(context.local()).FromJust());
  CHECK_EQ(42, CompileRun("func1.prototype = {}; func1.prototype.x")
                   ->Int32Value(context.local())
                   .FromJust());

  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->PrototypeTemplate()->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("func2"),
                  t2->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  // Default value of ReadOnly flag.
  CHECK(
      CompileRun(
          "(function() {"
          "  descriptor = Object.getOwnPropertyDescriptor(func2, 'prototype');"
          "  return (descriptor['writable'] == true);"
          "})()")
          ->BooleanValue(isolate));
  CHECK_EQ(
      42,
      CompileRun("func2.prototype.x")->Int32Value(context.local()).FromJust());
}


THREADED_TEST(SetPrototypeThrows) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);

  Local<v8::Object> o0 = t->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();
  Local<v8::Object> o1 = t->GetFunction(context.local())
                             .ToLocalChecked()
                             ->NewInstance(context.local())
                             .ToLocalChecked();

  CHECK(o0->SetPrototype(context.local(), o1).FromJust());
  // If setting the prototype leads to the cycle, SetPrototype should
  // return false and keep VM in sane state.
  v8::TryCatch try_catch(isolate);
  CHECK(o1->SetPrototype(context.local(), o0).IsNothing());
  CHECK(!try_catch.HasCaught());
  CHECK(!CcTest::i_isolate()->has_pending_exception());

  CHECK_EQ(42, CompileRun("function f() { return 42; }; f()")
                   ->Int32Value(context.local())
                   .FromJust());
}


THREADED_TEST(FunctionRemovePrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->RemovePrototype();
  Local<v8::Function> fun = t1->GetFunction(context.local()).ToLocalChecked();
  CHECK(!fun->IsConstructor());
  CHECK(context->Global()->Set(context.local(), v8_str("fun"), fun).FromJust());
  CHECK(!CompileRun("'prototype' in fun")->BooleanValue(isolate));

  v8::TryCatch try_catch(isolate);
  CompileRun("new fun()");
  CHECK(try_catch.HasCaught());

  try_catch.Reset();
  CHECK(fun->NewInstance(context.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(GetterSetterExceptions) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  CompileRun(
      "function Foo() { };"
      "function Throw() { throw 5; };"
      "var x = { };"
      "x.__defineSetter__('set', Throw);"
      "x.__defineGetter__('get', Throw);");
  Local<v8::Object> x = Local<v8::Object>::Cast(
      context->Global()->Get(context.local(), v8_str("x")).ToLocalChecked());
  v8::TryCatch try_catch(isolate);
  CHECK(x->Set(context.local(), v8_str("set"), v8::Integer::New(isolate, 8))
            .IsNothing());
  CHECK(x->Get(context.local(), v8_str("get")).IsEmpty());
  CHECK(x->Set(context.local(), v8_str("set"), v8::Integer::New(isolate, 8))
            .IsNothing());
  CHECK(x->Get(context.local(), v8_str("get")).IsEmpty());
  CHECK(x->Set(context.local(), v8_str("set"), v8::Integer::New(isolate, 8))
            .IsNothing());
  CHECK(x->Get(context.local(), v8_str("get")).IsEmpty());
  CHECK(x->Set(context.local(), v8_str("set"), v8::Integer::New(isolate, 8))
            .IsNothing());
  CHECK(x->Get(context.local(), v8_str("get")).IsEmpty());
}


THREADED_TEST(Constructor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("Fun"));
  Local<Function> cons = templ->GetFunction(context.local()).ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("Fun"), cons).FromJust());
  Local<v8::Object> inst = cons->NewInstance(context.local()).ToLocalChecked();
  i::Handle<i::JSReceiver> obj(v8::Utils::OpenHandle(*inst));
  CHECK(obj->IsJSObject());
  Local<Value> value = CompileRun("(new Fun()).constructor === Fun");
  CHECK(value->BooleanValue(isolate));
}


THREADED_TEST(FunctionDescriptorException) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("Fun"));
  Local<Function> cons = templ->GetFunction(context.local()).ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("Fun"), cons).FromJust());
  Local<Value> value = CompileRun(
      "function test() {"
      "  try {"
      "    (new Fun()).blah()"
      "  } catch (e) {"
      "    var str = String(e);"
      // "    if (str.indexOf('TypeError') == -1) return 1;"
      // "    if (str.indexOf('[object Fun]') != -1) return 2;"
      // "    if (str.indexOf('#<Fun>') == -1) return 3;"
      "    return 0;"
      "  }"
      "  return 4;"
      "}"
      "test();");
  CHECK_EQ(0, value->Int32Value(context.local()).FromJust());
}


THREADED_TEST(EvalAliasedDynamic) {
  LocalContext current;
  v8::HandleScope scope(current->GetIsolate());

  // Tests where aliased eval can only be resolved dynamically.
  Local<Script> script = v8_compile(
      "function f(x) { "
      "  var foo = 2;"
      "  with (x) { return eval('foo'); }"
      "}"
      "foo = 0;"
      "result1 = f(new Object());"
      "result2 = f(this);"
      "var x = new Object();"
      "x.eval = function(x) { return 1; };"
      "result3 = f(x);");
  script->Run(current.local()).ToLocalChecked();
  CHECK_EQ(2, current->Global()
                  ->Get(current.local(), v8_str("result1"))
                  .ToLocalChecked()
                  ->Int32Value(current.local())
                  .FromJust());
  CHECK_EQ(0, current->Global()
                  ->Get(current.local(), v8_str("result2"))
                  .ToLocalChecked()
                  ->Int32Value(current.local())
                  .FromJust());
  CHECK_EQ(1, current->Global()
                  ->Get(current.local(), v8_str("result3"))
                  .ToLocalChecked()
                  ->Int32Value(current.local())
                  .FromJust());

  v8::TryCatch try_catch(current->GetIsolate());
  script = v8_compile(
      "function f(x) { "
      "  var bar = 2;"
      "  with (x) { return eval('bar'); }"
      "}"
      "result4 = f(this)");
  script->Run(current.local()).ToLocalChecked();
  CHECK(!try_catch.HasCaught());
  CHECK_EQ(2, current->Global()
                  ->Get(current.local(), v8_str("result4"))
                  .ToLocalChecked()
                  ->Int32Value(current.local())
                  .FromJust());

  try_catch.Reset();
}


THREADED_TEST(CrossEval) {
  v8::HandleScope scope(CcTest::isolate());
  LocalContext other;
  LocalContext current;

  Local<String> token = v8_str("<security token>");
  other->SetSecurityToken(token);
  current->SetSecurityToken(token);

  // Set up reference from current to other.
  CHECK(current->Global()
            ->Set(current.local(), v8_str("other"), other->Global())
            .FromJust());

  // Check that new variables are introduced in other context.
  Local<Script> script = v8_compile("other.eval('var foo = 1234')");
  script->Run(current.local()).ToLocalChecked();
  Local<Value> foo =
      other->Global()->Get(current.local(), v8_str("foo")).ToLocalChecked();
  CHECK_EQ(1234, foo->Int32Value(other.local()).FromJust());
  CHECK(!current->Global()->Has(current.local(), v8_str("foo")).FromJust());

  // Check that writing to non-existing properties introduces them in
  // the other context.
  script = v8_compile("other.eval('na = 1234')");
  script->Run(current.local()).ToLocalChecked();
  CHECK_EQ(1234, other->Global()
                     ->Get(current.local(), v8_str("na"))
                     .ToLocalChecked()
                     ->Int32Value(other.local())
                     .FromJust());
  CHECK(!current->Global()->Has(current.local(), v8_str("na")).FromJust());

  // Check that global variables in current context are not visible in other
  // context.
  v8::TryCatch try_catch(CcTest::isolate());
  script = v8_compile("var bar = 42; other.eval('bar');");
  CHECK(script->Run(current.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that local variables in current context are not visible in other
  // context.
  script = v8_compile(
      "(function() { "
      "  var baz = 87;"
      "  return other.eval('baz');"
      "})();");
  CHECK(script->Run(current.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that global variables in the other environment are visible
  // when evaluting code.
  CHECK(other->Global()
            ->Set(other.local(), v8_str("bis"), v8_num(1234))
            .FromJust());
  script = v8_compile("other.eval('bis')");
  CHECK_EQ(1234, script->Run(current.local())
                     .ToLocalChecked()
                     ->Int32Value(current.local())
                     .FromJust());
  CHECK(!try_catch.HasCaught());

  // Check that the 'this' pointer points to the global object evaluating
  // code.
  CHECK(other->Global()
            ->Set(current.local(), v8_str("t"), other->Global())
            .FromJust());
  script = v8_compile("other.eval('this == t')");
  Local<Value> result = script->Run(current.local()).ToLocalChecked();
  CHECK(result->IsTrue());
  CHECK(!try_catch.HasCaught());

  // Check that variables introduced in with-statement are not visible in
  // other context.
  script = v8_compile("with({x:2}){other.eval('x')}");
  CHECK(script->Run(current.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that you cannot use 'eval.call' with another object than the
  // current global object.
  script = v8_compile("other.y = 1; eval.call(other, 'y')");
  CHECK(script->Run(current.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
}


// Test that calling eval in a context which has been detached from
// its global proxy works.
THREADED_TEST(EvalInDetachedGlobal) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<Context> context0 = Context::New(isolate);
  v8::Local<Context> context1 = Context::New(isolate);
  Local<String> token = v8_str("<security token>");
  context0->SetSecurityToken(token);
  context1->SetSecurityToken(token);

  // Set up function in context0 that uses eval from context0.
  context0->Enter();
  v8::Local<v8::Value> fun = CompileRun(
      "var x = 42;"
      "(function() {"
      "  var e = eval;"
      "  return function(s) { return e(s); }"
      "})()");
  context0->Exit();

  // Put the function into context1 and call it before and after
  // detaching the global.  Before detaching, the call succeeds and
  // after detaching undefined is returned.
  context1->Enter();
  CHECK(context1->Global()->Set(context1, v8_str("fun"), fun).FromJust());
  v8::Local<v8::Value> x_value = CompileRun("fun('x')");
  CHECK_EQ(42, x_value->Int32Value(context1).FromJust());
  context0->DetachGlobal();
  x_value = CompileRun("fun('x')");
  CHECK(x_value->IsUndefined());
  context1->Exit();
}


THREADED_TEST(CrossLazyLoad) {
  v8::HandleScope scope(CcTest::isolate());
  LocalContext other;
  LocalContext current;

  Local<String> token = v8_str("<security token>");
  other->SetSecurityToken(token);
  current->SetSecurityToken(token);

  // Set up reference from current to other.
  CHECK(current->Global()
            ->Set(current.local(), v8_str("other"), other->Global())
            .FromJust());

  // Trigger lazy loading in other context.
  Local<Script> script = v8_compile("other.eval('new Date(42)')");
  Local<Value> value = script->Run(current.local()).ToLocalChecked();
  CHECK_EQ(42.0, value->NumberValue(current.local()).FromJust());
}


static void call_as_function(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  if (args.IsConstructCall()) {
    if (args[0]->IsInt32()) {
      args.GetReturnValue().Set(
          v8_num(-args[0]
                      ->Int32Value(args.GetIsolate()->GetCurrentContext())
                      .FromJust()));
      return;
    }
  }

  args.GetReturnValue().Set(args[0]);
}


// Test that a call handler can be set for objects which will allow
// non-function objects created through the API to be called as
// functions.
THREADED_TEST(CallAsFunction) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(call_as_function);
    Local<v8::Object> instance = t->GetFunction(context.local())
                                     .ToLocalChecked()
                                     ->NewInstance(context.local())
                                     .ToLocalChecked();
    CHECK(context->Global()
              ->Set(context.local(), v8_str("obj"), instance)
              .FromJust());
    v8::TryCatch try_catch(isolate);
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    value = CompileRun("obj(42)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, value->Int32Value(context.local()).FromJust());

    value = CompileRun("(function(o){return o(49)})(obj)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(49, value->Int32Value(context.local()).FromJust());

    // test special case of call as function
    value = CompileRun("[obj]['0'](45)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(45, value->Int32Value(context.local()).FromJust());

    value = CompileRun(
        "obj.call = Function.prototype.call;"
        "obj.call(null, 87)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(87, value->Int32Value(context.local()).FromJust());

    // Regression tests for bug #1116356: Calling call through call/apply
    // must work for non-function receivers.
    const char* apply_99 = "Function.prototype.call.apply(obj, [this, 99])";
    value = CompileRun(apply_99);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(99, value->Int32Value(context.local()).FromJust());

    const char* call_17 = "Function.prototype.call.call(obj, this, 17)";
    value = CompileRun(call_17);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(17, value->Int32Value(context.local()).FromJust());

    // Check that the call-as-function handler can be called through new.
    value = CompileRun("new obj(43)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(-43, value->Int32Value(context.local()).FromJust());

    // Check that the call-as-function handler can be called through
    // the API.
    v8::Local<Value> args[] = {v8_num(28)};
    value = instance->CallAsFunction(context.local(), instance, 1, args)
                .ToLocalChecked();
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(28, value->Int32Value(context.local()).FromJust());
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template(t->InstanceTemplate());
    USE(instance_template);
    Local<v8::Object> instance = t->GetFunction(context.local())
                                     .ToLocalChecked()
                                     ->NewInstance(context.local())
                                     .ToLocalChecked();
    CHECK(context->Global()
              ->Set(context.local(), v8_str("obj2"), instance)
              .FromJust());
    v8::TryCatch try_catch(isolate);
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    // Call an object without call-as-function handler through the JS
    value = CompileRun("obj2(28)");
    CHECK(value.IsEmpty());
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(isolate, try_catch.Exception());
    // TODO(verwaest): Better message
    CHECK_EQ(0, strcmp("TypeError: obj2 is not a function", *exception_value1));
    try_catch.Reset();

    // Call an object without call-as-function handler through the API
    v8::Local<Value> args[] = {v8_num(28)};
    CHECK(
        instance->CallAsFunction(context.local(), instance, 1, args).IsEmpty());
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(isolate, try_catch.Exception());
    CHECK_EQ(0,
             strcmp("TypeError: object is not a function", *exception_value2));
    try_catch.Reset();
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(ThrowValue);
    Local<v8::Object> instance = t->GetFunction(context.local())
                                     .ToLocalChecked()
                                     ->NewInstance(context.local())
                                     .ToLocalChecked();
    CHECK(context->Global()
              ->Set(context.local(), v8_str("obj3"), instance)
              .FromJust());
    v8::TryCatch try_catch(isolate);
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    // Catch the exception which is thrown by call-as-function handler
    value = CompileRun("obj3(22)");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(isolate, try_catch.Exception());
    CHECK_EQ(0, strcmp("22", *exception_value1));
    try_catch.Reset();

    v8::Local<Value> args[] = {v8_num(23)};
    CHECK(
        instance->CallAsFunction(context.local(), instance, 1, args).IsEmpty());
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(isolate, try_catch.Exception());
    CHECK_EQ(0, strcmp("23", *exception_value2));
    try_catch.Reset();
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(ReturnThis);
    Local<v8::Object> instance = t->GetFunction(context.local())
                                     .ToLocalChecked()
                                     ->NewInstance(context.local())
                                     .ToLocalChecked();

    Local<v8::Value> a1 =
        instance
            ->CallAsFunction(context.local(), v8::Undefined(isolate), 0,
                             nullptr)
            .ToLocalChecked();
    CHECK(a1->StrictEquals(instance));
    Local<v8::Value> a2 =
        instance->CallAsFunction(context.local(), v8::Null(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a2->StrictEquals(instance));
    Local<v8::Value> a3 =
        instance->CallAsFunction(context.local(), v8_num(42), 0, nullptr)
            .ToLocalChecked();
    CHECK(a3->StrictEquals(instance));
    Local<v8::Value> a4 =
        instance->CallAsFunction(context.local(), v8_str("hello"), 0, nullptr)
            .ToLocalChecked();
    CHECK(a4->StrictEquals(instance));
    Local<v8::Value> a5 =
        instance->CallAsFunction(context.local(), v8::True(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a5->StrictEquals(instance));
  }

  {
    CompileRun(
        "function ReturnThisSloppy() {"
        "  return this;"
        "}"
        "function ReturnThisStrict() {"
        "  'use strict';"
        "  return this;"
        "}");
    Local<Function> ReturnThisSloppy = Local<Function>::Cast(
        context->Global()
            ->Get(context.local(), v8_str("ReturnThisSloppy"))
            .ToLocalChecked());
    Local<Function> ReturnThisStrict = Local<Function>::Cast(
        context->Global()
            ->Get(context.local(), v8_str("ReturnThisStrict"))
            .ToLocalChecked());

    Local<v8::Value> a1 =
        ReturnThisSloppy
            ->CallAsFunction(context.local(), v8::Undefined(isolate), 0,
                             nullptr)
            .ToLocalChecked();
    CHECK(a1->StrictEquals(context->Global()));
    Local<v8::Value> a2 =
        ReturnThisSloppy
            ->CallAsFunction(context.local(), v8::Null(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a2->StrictEquals(context->Global()));
    Local<v8::Value> a3 =
        ReturnThisSloppy
            ->CallAsFunction(context.local(), v8_num(42), 0, nullptr)
            .ToLocalChecked();
    CHECK(a3->IsNumberObject());
    CHECK_EQ(42.0, a3.As<v8::NumberObject>()->ValueOf());
    Local<v8::Value> a4 =
        ReturnThisSloppy
            ->CallAsFunction(context.local(), v8_str("hello"), 0, nullptr)
            .ToLocalChecked();
    CHECK(a4->IsStringObject());
    CHECK(a4.As<v8::StringObject>()->ValueOf()->StrictEquals(v8_str("hello")));
    Local<v8::Value> a5 =
        ReturnThisSloppy
            ->CallAsFunction(context.local(), v8::True(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a5->IsBooleanObject());
    CHECK(a5.As<v8::BooleanObject>()->ValueOf());

    Local<v8::Value> a6 =
        ReturnThisStrict
            ->CallAsFunction(context.local(), v8::Undefined(isolate), 0,
                             nullptr)
            .ToLocalChecked();
    CHECK(a6->IsUndefined());
    Local<v8::Value> a7 =
        ReturnThisStrict
            ->CallAsFunction(context.local(), v8::Null(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a7->IsNull());
    Local<v8::Value> a8 =
        ReturnThisStrict
            ->CallAsFunction(context.local(), v8_num(42), 0, nullptr)
            .ToLocalChecked();
    CHECK(a8->StrictEquals(v8_num(42)));
    Local<v8::Value> a9 =
        ReturnThisStrict
            ->CallAsFunction(context.local(), v8_str("hello"), 0, nullptr)
            .ToLocalChecked();
    CHECK(a9->StrictEquals(v8_str("hello")));
    Local<v8::Value> a10 =
        ReturnThisStrict
            ->CallAsFunction(context.local(), v8::True(isolate), 0, nullptr)
            .ToLocalChecked();
    CHECK(a10->StrictEquals(v8::True(isolate)));
  }
}


// Check whether a non-function object is callable.
THREADED_TEST(CallableObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    instance_template->SetCallAsFunctionHandler(call_as_function);
    Local<Object> instance =
        instance_template->NewInstance(context.local()).ToLocalChecked();
    v8::TryCatch try_catch(isolate);

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    Local<Object> instance =
        instance_template->NewInstance(context.local()).ToLocalChecked();
    v8::TryCatch try_catch(isolate);

    CHECK(!instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<FunctionTemplate> function_template =
        FunctionTemplate::New(isolate, call_as_function);
    Local<Function> function =
        function_template->GetFunction(context.local()).ToLocalChecked();
    Local<Object> instance = function;
    v8::TryCatch try_catch(isolate);

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<FunctionTemplate> function_template = FunctionTemplate::New(isolate);
    Local<Function> function =
        function_template->GetFunction(context.local()).ToLocalChecked();
    Local<Object> instance = function;
    v8::TryCatch try_catch(isolate);

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }
}


THREADED_TEST(Regress567998) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<v8::FunctionTemplate> desc =
      v8::FunctionTemplate::New(env->GetIsolate());
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable
  desc->InstanceTemplate()->SetCallAsFunctionHandler(ReturnThis);  // callable

  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();
  CHECK(
      env->Global()->Set(env.local(), v8_str("undetectable"), obj).FromJust());

  ExpectString("undetectable.toString()", "[object Object]");
  ExpectString("typeof undetectable", "undefined");
  ExpectString("typeof(undetectable)", "undefined");
  ExpectBoolean("typeof undetectable == 'undefined'", true);
  ExpectBoolean("typeof undetectable == 'object'", false);
  ExpectBoolean("if (undetectable) { true; } else { false; }", false);
  ExpectBoolean("!undetectable", true);

  ExpectObject("true&&undetectable", obj);
  ExpectBoolean("false&&undetectable", false);
  ExpectBoolean("true||undetectable", true);
  ExpectObject("false||undetectable", obj);

  ExpectObject("undetectable&&true", obj);
  ExpectObject("undetectable&&false", obj);
  ExpectBoolean("undetectable||true", true);
  ExpectBoolean("undetectable||false", false);

  ExpectBoolean("undetectable==null", true);
  ExpectBoolean("null==undetectable", true);
  ExpectBoolean("undetectable==undefined", true);
  ExpectBoolean("undefined==undetectable", true);
  ExpectBoolean("undetectable==undetectable", true);

  ExpectBoolean("undetectable===null", false);
  ExpectBoolean("null===undetectable", false);
  ExpectBoolean("undetectable===undefined", false);
  ExpectBoolean("undefined===undetectable", false);
  ExpectBoolean("undetectable===undetectable", true);
}


static int Recurse(v8::Isolate* isolate, int depth, int iterations) {
  v8::HandleScope scope(isolate);
  if (depth == 0) return v8::HandleScope::NumberOfHandles(isolate);
  for (int i = 0; i < iterations; i++) {
    Local<v8::Number> n(v8::Integer::New(isolate, 42));
  }
  return Recurse(isolate, depth - 1, iterations);
}


THREADED_TEST(HandleIteration) {
  static const int kIterations = 500;
  static const int kNesting = 200;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope0(isolate);
  CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
  {
    v8::HandleScope scope1(isolate);
    CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
    for (int i = 0; i < kIterations; i++) {
      Local<v8::Number> n(v8::Integer::New(CcTest::isolate(), 42));
      CHECK_EQ(i + 1, v8::HandleScope::NumberOfHandles(isolate));
    }

    CHECK_EQ(kIterations, v8::HandleScope::NumberOfHandles(isolate));
    {
      v8::HandleScope scope2(CcTest::isolate());
      for (int j = 0; j < kIterations; j++) {
        Local<v8::Number> n(v8::Integer::New(CcTest::isolate(), 42));
        CHECK_EQ(j + 1 + kIterations,
                 v8::HandleScope::NumberOfHandles(isolate));
      }
    }
    CHECK_EQ(kIterations, v8::HandleScope::NumberOfHandles(isolate));
  }
  CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
  CHECK_EQ(kNesting * kIterations, Recurse(isolate, kNesting, kIterations));
}


static void InterceptorCallICFastApi(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(InterceptorCallICFastApi));
  int* call_count =
      reinterpret_cast<int*>(v8::External::Cast(*info.Data())->Value());
  ++(*call_count);
  if ((*call_count) % 20 == 0) {
    CcTest::CollectAllGarbage();
  }
}

static void FastApiCallback_TrivialSignature(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(args, FUNCTION_ADDR(FastApiCallback_TrivialSignature));
  v8::Isolate* isolate = CcTest::isolate();
  CHECK_EQ(isolate, args.GetIsolate());
  CHECK(args.This()
            ->Equals(isolate->GetCurrentContext(), args.Holder())
            .FromJust());
  CHECK(args.Data()
            ->Equals(isolate->GetCurrentContext(), v8_str("method_data"))
            .FromJust());
  args.GetReturnValue().Set(
      args[0]->Int32Value(isolate->GetCurrentContext()).FromJust() + 1);
}

static void FastApiCallback_SimpleSignature(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(args, FUNCTION_ADDR(FastApiCallback_SimpleSignature));
  v8::Isolate* isolate = CcTest::isolate();
  CHECK_EQ(isolate, args.GetIsolate());
  CHECK(args.This()
            ->GetPrototype()
            ->Equals(isolate->GetCurrentContext(), args.Holder())
            .FromJust());
  CHECK(args.Data()
            ->Equals(isolate->GetCurrentContext(), v8_str("method_data"))
            .FromJust());
  // Note, we're using HasRealNamedProperty instead of Has to avoid
  // invoking the interceptor again.
  CHECK(args.Holder()
            ->HasRealNamedProperty(isolate->GetCurrentContext(), v8_str("foo"))
            .FromJust());
  args.GetReturnValue().Set(
      args[0]->Int32Value(isolate->GetCurrentContext()).FromJust() + 1);
}


// Helper to maximize the odds of object moving.
static void GenerateSomeGarbage() {
  CompileRun(
      "var garbage;"
      "for (var i = 0; i < 1000; i++) {"
      "  garbage = [1/i, \"garbage\" + i, garbage, {foo: garbage}];"
      "}"
      "garbage = undefined;");
}


void DirectApiCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  static int count = 0;
  if (count++ % 3 == 0) {
    CcTest::CollectAllGarbage();
    // This should move the stub
    GenerateSomeGarbage();  // This should ensure the old stub memory is flushed
  }
}


THREADED_TEST(CallICFastApi_DirectCall_GCMoveStub) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> nativeobject_templ =
      v8::ObjectTemplate::New(isolate);
  nativeobject_templ->Set(isolate, "callback",
                          v8::FunctionTemplate::New(isolate,
                                                    DirectApiCallback));
  v8::Local<v8::Object> nativeobject_obj =
      nativeobject_templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("nativeobject"), nativeobject_obj)
            .FromJust());
  // call the api function multiple times to ensure direct call stub creation.
  CompileRun(
      "function f() {"
      "  for (var i = 1; i <= 30; i++) {"
      "    nativeobject.callback();"
      "  }"
      "}"
      "f();");
}

void ThrowingDirectApiCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetIsolate()->ThrowException(v8_str("g"));
}

THREADED_TEST(CallICFastApi_DirectCall_Throw) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> nativeobject_templ =
      v8::ObjectTemplate::New(isolate);
  nativeobject_templ->Set(
      isolate, "callback",
      v8::FunctionTemplate::New(isolate, ThrowingDirectApiCallback));
  v8::Local<v8::Object> nativeobject_obj =
      nativeobject_templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("nativeobject"), nativeobject_obj)
            .FromJust());
  // call the api function multiple times to ensure direct call stub creation.
  v8::Local<Value> result = CompileRun(
      "var result = '';"
      "function f() {"
      "  for (var i = 1; i <= 5; i++) {"
      "    try { nativeobject.callback(); } catch (e) { result += e; }"
      "  }"
      "}"
      "f(); result;");
  CHECK(v8_str("ggggg")->Equals(context.local(), result).FromJust());
}

static int p_getter_count_3;

static Local<Value> DoDirectGetter() {
  if (++p_getter_count_3 % 3 == 0) {
    CcTest::CollectAllGarbage();
    GenerateSomeGarbage();
  }
  return v8_str("Direct Getter Result");
}

static void DirectGetterCallback(
    Local<String> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, FUNCTION_ADDR(DirectGetterCallback));
  info.GetReturnValue().Set(DoDirectGetter());
}

template <typename Accessor>
static void LoadICFastApi_DirectCall_GCMoveStub(Accessor accessor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = v8::ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), accessor);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o1"),
                  obj->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  p_getter_count_3 = 0;
  v8::Local<v8::Value> result = CompileRun(
      "function f() {"
      "  for (var i = 0; i < 30; i++) o1.p1;"
      "  return o1.p1"
      "}"
      "f();");
  CHECK(v8_str("Direct Getter Result")
            ->Equals(context.local(), result)
            .FromJust());
  CHECK_EQ(31, p_getter_count_3);
}

THREADED_PROFILED_TEST(LoadICFastApi_DirectCall_GCMoveStub) {
  LoadICFastApi_DirectCall_GCMoveStub(DirectGetterCallback);
}

void ThrowingDirectGetterCallback(
    Local<String> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->ThrowException(v8_str("g"));
}

THREADED_TEST(LoadICFastApi_DirectCall_Throw) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> obj = v8::ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), ThrowingDirectGetterCallback);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o1"),
                  obj->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  v8::Local<Value> result = CompileRun(
      "var result = '';"
      "for (var i = 0; i < 5; i++) {"
      "    try { o1.p1; } catch (e) { result += e; }"
      "}"
      "result;");
  CHECK(v8_str("ggggg")->Equals(context.local(), result).FromJust());
}

THREADED_PROFILED_TEST(InterceptorCallICFastApi_TrivialSignature) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Local<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_TrivialSignature, v8_str("method_data"),
      v8::Local<v8::Signature>());
  v8::Local<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  v8::Local<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, nullptr, nullptr, nullptr, nullptr,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Local<v8::Function> fun =
      fun_templ->GetFunction(context.local()).ToLocalChecked();
  GenerateSomeGarbage();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o"),
                  fun->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun(
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = o.method(41);"
      "}");
  CHECK_EQ(42, context->Global()
                   ->Get(context.local(), v8_str("result"))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
  CHECK_EQ(100, interceptor_call_count);
}

THREADED_PROFILED_TEST(CallICFastApi_TrivialSignature) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Local<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_TrivialSignature, v8_str("method_data"),
      v8::Local<v8::Signature>());
  v8::Local<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  v8::Local<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  USE(templ);
  LocalContext context;
  v8::Local<v8::Function> fun =
      fun_templ->GetFunction(context.local()).ToLocalChecked();
  GenerateSomeGarbage();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o"),
                  fun->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun(
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = o.method(41);"
      "}");

  CHECK_EQ(42, context->Global()
                   ->Get(context.local(), v8_str("result"))
                   .ToLocalChecked()
                   ->Int32Value(context.local())
                   .FromJust());
}

static void ThrowingGetter(Local<String> name,
                           const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetIsolate()->ThrowException(Local<Value>());
  info.GetReturnValue().SetUndefined();
}


THREADED_TEST(VariousGetPropertiesAndThrowingCallbacks) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetAccessor(v8_str("f"), ThrowingGetter);

  Local<Object> instance = templ->GetFunction(context.local())
                               .ToLocalChecked()
                               ->NewInstance(context.local())
                               .ToLocalChecked();

  Local<Object> another = Object::New(context->GetIsolate());
  CHECK(another->SetPrototype(context.local(), instance).FromJust());

  Local<Object> with_js_getter = CompileRun(
      "o = {};\n"
      "o.__defineGetter__('f', function() { throw undefined; });\n"
      "o\n").As<Object>();
  CHECK(!with_js_getter.IsEmpty());

  TryCatch try_catch(context->GetIsolate());

  v8::MaybeLocal<Value> result =
      instance->GetRealNamedProperty(context.local(), v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  Maybe<PropertyAttribute> attr =
      instance->GetRealNamedPropertyAttributes(context.local(), v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->GetRealNamedProperty(context.local(), v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = another->GetRealNamedPropertyAttributes(context.local(), v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->GetRealNamedPropertyInPrototypeChain(context.local(),
                                                         v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = another->GetRealNamedPropertyAttributesInPrototypeChain(
      context.local(), v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->Get(context.local(), v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  result = with_js_getter->GetRealNamedProperty(context.local(), v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = with_js_getter->GetRealNamedPropertyAttributes(context.local(),
                                                        v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = with_js_getter->Get(context.local(), v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  Local<Object> target = CompileRun("({})").As<Object>();
  Local<Object> handler = CompileRun("({})").As<Object>();
  Local<v8::Proxy> proxy =
      v8::Proxy::New(context.local(), target, handler).ToLocalChecked();

  result = target->GetRealNamedProperty(context.local(), v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(result.IsEmpty());

  result = proxy->GetRealNamedProperty(context.local(), v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(result.IsEmpty());
}


static void ThrowingCallbackWithTryCatch(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TryCatch try_catch(args.GetIsolate());
  // Verboseness is important: it triggers message delivery which can call into
  // external code.
  try_catch.SetVerbose(true);
  CompileRun("throw 'from JS';");
  CHECK(try_catch.HasCaught());
  CHECK(!CcTest::i_isolate()->has_pending_exception());
  CHECK(!CcTest::i_isolate()->has_scheduled_exception());
}


static int call_depth;


static void WithTryCatch(Local<Message> message, Local<Value> data) {
  TryCatch try_catch(CcTest::isolate());
}


static void ThrowFromJS(Local<Message> message, Local<Value> data) {
  if (--call_depth) CompileRun("throw 'ThrowInJS';");
}


static void ThrowViaApi(Local<Message> message, Local<Value> data) {
  if (--call_depth) CcTest::isolate()->ThrowException(v8_str("ThrowViaApi"));
}


static void WebKitLike(Local<Message> message, Local<Value> data) {
  Local<String> errorMessageString = message->Get();
  CHECK(!errorMessageString.IsEmpty());
  message->GetStackTrace();
  message->GetScriptOrigin().ResourceName();
}


THREADED_TEST(ExceptionsDoNotPropagatePastTryCatch) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  HandleScope scope(isolate);

  Local<Function> func =
      FunctionTemplate::New(isolate, ThrowingCallbackWithTryCatch)
          ->GetFunction(context.local())
          .ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("func"), func).FromJust());

  MessageCallback callbacks[] = {nullptr, WebKitLike, ThrowViaApi, ThrowFromJS,
                                 WithTryCatch};
  for (unsigned i = 0; i < sizeof(callbacks)/sizeof(callbacks[0]); i++) {
    MessageCallback callback = callbacks[i];
    if (callback != nullptr) {
      isolate->AddMessageListener(callback);
    }
    // Some small number to control number of times message handler should
    // throw an exception.
    call_depth = 5;
    ExpectFalse(
        "var thrown = false;\n"
        "try { func(); } catch(e) { thrown = true; }\n"
        "thrown\n");
    if (callback != nullptr) {
      isolate->RemoveMessageListeners(callback);
    }
  }
}


static void ParentGetter(Local<String> name,
                         const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(1));
}


static void ChildGetter(Local<String> name,
                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(42));
}


THREADED_TEST(Overriding) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  // Parent template.
  Local<v8::FunctionTemplate> parent_templ = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> parent_instance_templ =
      parent_templ->InstanceTemplate();
  parent_instance_templ->SetAccessor(v8_str("f"), ParentGetter);

  // Template that inherits from the parent template.
  Local<v8::FunctionTemplate> child_templ = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> child_instance_templ =
      child_templ->InstanceTemplate();
  child_templ->Inherit(parent_templ);
  // Override 'f'.  The child version of 'f' should get called for child
  // instances.
  child_instance_templ->SetAccessor(v8_str("f"), ChildGetter);
  // Add 'g' twice.  The 'g' added last should get called for instances.
  child_instance_templ->SetAccessor(v8_str("g"), ParentGetter);
  child_instance_templ->SetAccessor(v8_str("g"), ChildGetter);

  // Add 'h' as an accessor to the proto template with ReadOnly attributes
  // so 'h' can be shadowed on the instance object.
  Local<ObjectTemplate> child_proto_templ = child_templ->PrototypeTemplate();
  child_proto_templ->SetAccessor(v8_str("h"), ParentGetter, nullptr,
                                 v8::Local<Value>(), v8::DEFAULT, v8::ReadOnly);

  // Add 'i' as an accessor to the instance template with ReadOnly attributes
  // but the attribute does not have effect because it is duplicated with
  // nullptr setter.
  child_instance_templ->SetAccessor(v8_str("i"), ChildGetter, nullptr,
                                    v8::Local<Value>(), v8::DEFAULT,
                                    v8::ReadOnly);

  // Instantiate the child template.
  Local<v8::Object> instance = child_templ->GetFunction(context.local())
                                   .ToLocalChecked()
                                   ->NewInstance(context.local())
                                   .ToLocalChecked();

  // Check that the child function overrides the parent one.
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o"), instance)
            .FromJust());
  Local<Value> value = v8_compile("o.f")->Run(context.local()).ToLocalChecked();
  // Check that the 'g' that was added last is hit.
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());
  value = v8_compile("o.g")->Run(context.local()).ToLocalChecked();
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());

  // Check that 'h' cannot be shadowed.
  value = v8_compile("o.h = 3; o.h")->Run(context.local()).ToLocalChecked();
  CHECK_EQ(1, value->Int32Value(context.local()).FromJust());

  // Check that 'i' cannot be shadowed or changed.
  value = v8_compile("o.i = 3; o.i")->Run(context.local()).ToLocalChecked();
  CHECK_EQ(42, value->Int32Value(context.local()).FromJust());
}


static void ShouldThrowOnErrorGetter(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = info.GetIsolate();
  Local<Boolean> should_throw_on_error =
      Boolean::New(isolate, info.ShouldThrowOnError());
  info.GetReturnValue().Set(should_throw_on_error);
}


template <typename T>
static void ShouldThrowOnErrorSetter(Local<Name> name, Local<v8::Value> value,
                                     const v8::PropertyCallbackInfo<T>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = info.GetIsolate();
  auto context = isolate->GetCurrentContext();
  Local<Boolean> should_throw_on_error_value =
      Boolean::New(isolate, info.ShouldThrowOnError());
  CHECK(context->Global()
            ->Set(isolate->GetCurrentContext(), v8_str("should_throw_setter"),
                  should_throw_on_error_value)
            .FromJust());
}


THREADED_TEST(AccessorShouldThrowOnError) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetAccessor(v8_str("f"), ShouldThrowOnErrorGetter,
                              ShouldThrowOnErrorSetter<void>);

  Local<v8::Object> instance = templ->GetFunction(context.local())
                                   .ToLocalChecked()
                                   ->NewInstance(context.local())
                                   .ToLocalChecked();

  CHECK(global->Set(context.local(), v8_str("o"), instance).FromJust());

  // SLOPPY mode
  Local<Value> value = v8_compile("o.f")->Run(context.local()).ToLocalChecked();
  CHECK(value->IsFalse());
  v8_compile("o.f = 153")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_setter"))
              .ToLocalChecked();
  CHECK(value->IsFalse());

  // STRICT mode
  value = v8_compile("'use strict';o.f")->Run(context.local()).ToLocalChecked();
  CHECK(value->IsFalse());
  v8_compile("'use strict'; o.f = 153")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_setter"))
              .ToLocalChecked();
  CHECK(value->IsTrue());
}


static void ShouldThrowOnErrorQuery(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = info.GetIsolate();
  info.GetReturnValue().Set(v8::None);

  auto context = isolate->GetCurrentContext();
  Local<Boolean> should_throw_on_error_value =
      Boolean::New(isolate, info.ShouldThrowOnError());
  CHECK(context->Global()
            ->Set(isolate->GetCurrentContext(), v8_str("should_throw_query"),
                  should_throw_on_error_value)
            .FromJust());
}


static void ShouldThrowOnErrorDeleter(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Boolean>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = info.GetIsolate();
  info.GetReturnValue().Set(v8::True(isolate));

  auto context = isolate->GetCurrentContext();
  Local<Boolean> should_throw_on_error_value =
      Boolean::New(isolate, info.ShouldThrowOnError());
  CHECK(context->Global()
            ->Set(isolate->GetCurrentContext(), v8_str("should_throw_deleter"),
                  should_throw_on_error_value)
            .FromJust());
}


static void ShouldThrowOnErrorPropertyEnumerator(
    const v8::PropertyCallbackInfo<v8::Array>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = info.GetIsolate();
  Local<v8::Array> names = v8::Array::New(isolate, 1);
  CHECK(names->Set(isolate->GetCurrentContext(), names, v8_num(1)).FromJust());
  info.GetReturnValue().Set(names);

  auto context = isolate->GetCurrentContext();
  Local<Boolean> should_throw_on_error_value =
      Boolean::New(isolate, info.ShouldThrowOnError());
  CHECK(context->Global()
            ->Set(isolate->GetCurrentContext(),
                  v8_str("should_throw_enumerator"),
                  should_throw_on_error_value)
            .FromJust());
}


THREADED_TEST(InterceptorShouldThrowOnError) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();

  auto interceptor_templ = v8::ObjectTemplate::New(isolate);
  v8::NamedPropertyHandlerConfiguration handler(
      ShouldThrowOnErrorGetter, ShouldThrowOnErrorSetter<Value>,
      ShouldThrowOnErrorQuery, ShouldThrowOnErrorDeleter,
      ShouldThrowOnErrorPropertyEnumerator);
  interceptor_templ->SetHandler(handler);

  Local<v8::Object> instance =
      interceptor_templ->NewInstance(context.local()).ToLocalChecked();

  CHECK(global->Set(context.local(), v8_str("o"), instance).FromJust());

  // SLOPPY mode
  Local<Value> value = v8_compile("o.f")->Run(context.local()).ToLocalChecked();
  CHECK(value->IsFalse());
  v8_compile("o.f = 153")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_setter"))
              .ToLocalChecked();
  CHECK(value->IsFalse());

  v8_compile("delete o.f")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_deleter"))
              .ToLocalChecked();
  CHECK(value->IsFalse());

  v8_compile("Object.getOwnPropertyNames(o)")
      ->Run(context.local())
      .ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_enumerator"))
              .ToLocalChecked();
  CHECK(value->IsFalse());

  // STRICT mode
  value = v8_compile("'use strict';o.f")->Run(context.local()).ToLocalChecked();
  CHECK(value->IsFalse());
  v8_compile("'use strict'; o.f = 153")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_setter"))
              .ToLocalChecked();
  CHECK(value->IsTrue());

  v8_compile("'use strict'; delete o.f")->Run(context.local()).ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_deleter"))
              .ToLocalChecked();
  CHECK(value->IsTrue());

  v8_compile("'use strict'; Object.getOwnPropertyNames(o)")
      ->Run(context.local())
      .ToLocalChecked();
  value = global->Get(context.local(), v8_str("should_throw_enumerator"))
              .ToLocalChecked();
  CHECK(value->IsFalse());
}

static void EmptyHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {}

TEST(CallHandlerHasNoSideEffect) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetCallHandler(EmptyHandler);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("new f()"), true).IsEmpty());

  // Side-effect-free version.
  Local<v8::FunctionTemplate> templ2 = v8::FunctionTemplate::New(isolate);
  templ2->SetCallHandler(EmptyHandler, v8::Local<Value>(),
                         v8::SideEffectType::kHasNoSideEffect);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f2"),
                  templ2->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), true).ToLocalChecked();
  v8::debug::EvaluateGlobal(isolate, v8_str("new f2()"), true).ToLocalChecked();
}

TEST(FunctionTemplateNewHasNoSideEffect) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ =
      v8::FunctionTemplate::New(isolate, EmptyHandler);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("new f()"), true).IsEmpty());

  // Side-effect-free version.
  Local<v8::FunctionTemplate> templ2 = v8::FunctionTemplate::New(
      isolate, EmptyHandler, v8::Local<Value>(), v8::Local<v8::Signature>(), 0,
      v8::ConstructorBehavior::kAllow, v8::SideEffectType::kHasNoSideEffect);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f2"),
                  templ2->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), true).ToLocalChecked();
  v8::debug::EvaluateGlobal(isolate, v8_str("new f2()"), true).ToLocalChecked();
}

TEST(FunctionTemplateNewWithCacheHasNoSideEffect) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  v8::Local<v8::Private> priv =
      v8::Private::ForApi(isolate, v8_str("Foo#draft"));

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ =
      v8::FunctionTemplate::NewWithCache(isolate, EmptyHandler, priv);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("new f()"), true).IsEmpty());

  // Side-effect-free version.
  Local<v8::FunctionTemplate> templ2 = v8::FunctionTemplate::NewWithCache(
      isolate, EmptyHandler, priv, v8::Local<Value>(),
      v8::Local<v8::Signature>(), 0, v8::SideEffectType::kHasNoSideEffect);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f2"),
                  templ2->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), true).ToLocalChecked();
  v8::debug::EvaluateGlobal(isolate, v8_str("new f2()"), true).ToLocalChecked();
}

TEST(FunctionNewHasNoSideEffect) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;

  // Function with side-effect.
  Local<Function> func =
      Function::New(context.local(), EmptyHandler).ToLocalChecked();
  CHECK(context->Global()->Set(context.local(), v8_str("f"), func).FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("new f()"), true).IsEmpty());

  // Side-effect-free version.
  Local<Function> func2 =
      Function::New(context.local(), EmptyHandler, Local<Value>(), 0,
                    v8::ConstructorBehavior::kAllow,
                    v8::SideEffectType::kHasNoSideEffect)
          .ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("f2"), func2).FromJust());
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), true).ToLocalChecked();
  v8::debug::EvaluateGlobal(isolate, v8_str("new f2()"), true).ToLocalChecked();
}

// These handlers instantiate a function the embedder considers safe in some
// cases (e.g. "building object wrappers"), but those functions themselves were
// not explicitly marked as side-effect-free.
static void DefaultConstructHandler(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Context::Scope context_scope(context);
  v8::MaybeLocal<v8::Object> instance = Function::New(context, EmptyHandler)
                                            .ToLocalChecked()
                                            ->NewInstance(context, 0, nullptr);
  USE(instance);
}

static void NoSideEffectConstructHandler(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Context::Scope context_scope(context);
  v8::MaybeLocal<v8::Object> instance =
      Function::New(context, EmptyHandler)
          .ToLocalChecked()
          ->NewInstanceWithSideEffectType(context, 0, nullptr,
                                          v8::SideEffectType::kHasNoSideEffect);
  USE(instance);
}

static void NoSideEffectAndSideEffectConstructHandler(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Context::Scope context_scope(context);
  // Constructs an instance in a side-effect-free way, followed by another with
  // side effects.
  v8::MaybeLocal<v8::Object> instance =
      Function::New(context, EmptyHandler)
          .ToLocalChecked()
          ->NewInstanceWithSideEffectType(context, 0, nullptr,
                                          v8::SideEffectType::kHasNoSideEffect);
  v8::MaybeLocal<v8::Object> instance2 = Function::New(context, EmptyHandler)
                                             .ToLocalChecked()
                                             ->NewInstance(context, 0, nullptr);
  USE(instance);
  USE(instance2);
}

TEST(FunctionNewInstanceHasNoSideEffect) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;

  // A whitelisted function that creates a new object with both side-effect
  // free/full instantiations. Should throw.
  Local<Function> func0 =
      Function::New(context.local(), NoSideEffectAndSideEffectConstructHandler,
                    Local<Value>(), 0, v8::ConstructorBehavior::kAllow,
                    v8::SideEffectType::kHasNoSideEffect)
          .ToLocalChecked();
  CHECK(context->Global()->Set(context.local(), v8_str("f"), func0).FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());

  // A whitelisted function that creates a new object. Should throw.
  Local<Function> func =
      Function::New(context.local(), DefaultConstructHandler, Local<Value>(), 0,
                    v8::ConstructorBehavior::kAllow,
                    v8::SideEffectType::kHasNoSideEffect)
          .ToLocalChecked();
  CHECK(context->Global()->Set(context.local(), v8_str("f"), func).FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f()"), true).IsEmpty());

  // A whitelisted function that creates a new object with explicit intent to
  // have no side-effects (e.g. building an "object wrapper"). Should not throw.
  Local<Function> func2 =
      Function::New(context.local(), NoSideEffectConstructHandler,
                    Local<Value>(), 0, v8::ConstructorBehavior::kAllow,
                    v8::SideEffectType::kHasNoSideEffect)
          .ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("f2"), func2).FromJust());
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), true).ToLocalChecked();

  // Check that side effect skipping did not leak outside to future evaluations.
  Local<Function> func3 =
      Function::New(context.local(), EmptyHandler).ToLocalChecked();
  CHECK(
      context->Global()->Set(context.local(), v8_str("f3"), func3).FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("f3()"), true).IsEmpty());

  // Check that using side effect free NewInstance works in normal evaluation
  // (without throwOnSideEffect).
  v8::debug::EvaluateGlobal(isolate, v8_str("f2()"), false).ToLocalChecked();
}

TEST(CallHandlerAsFunctionHasNoSideEffectNotSupported) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;

  // Object template with call as function handler.
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetCallAsFunctionHandler(EmptyHandler);
  Local<v8::Object> obj = templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()->Set(context.local(), v8_str("obj"), obj).FromJust());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("obj()"), true).IsEmpty());

  // Side-effect-free version is not supported.
  i::FunctionTemplateInfo cons = i::FunctionTemplateInfo::cast(
      v8::Utils::OpenHandle(*templ)->constructor());
  i::Heap* heap = reinterpret_cast<i::Isolate*>(isolate)->heap();
  i::CallHandlerInfo handler_info =
      i::CallHandlerInfo::cast(cons.GetInstanceCallHandler());
  CHECK(!handler_info.IsSideEffectFreeCallHandlerInfo());
  handler_info.set_map(
      i::ReadOnlyRoots(heap).side_effect_free_call_handler_info_map());
  CHECK(v8::debug::EvaluateGlobal(isolate, v8_str("obj()"), true).IsEmpty());
}

static void IsConstructHandler(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(args.IsConstructCall());
}


THREADED_TEST(IsConstructCall) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetCallHandler(IsConstructHandler);

  LocalContext context;

  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());
  Local<Value> value = v8_compile("f()")->Run(context.local()).ToLocalChecked();
  CHECK(!value->BooleanValue(isolate));
  value = v8_compile("new f()")->Run(context.local()).ToLocalChecked();
  CHECK(value->BooleanValue(isolate));
}

static void NewTargetHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(args.NewTarget());
}

THREADED_TEST(NewTargetHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetCallHandler(NewTargetHandler);

  LocalContext context;

  Local<Function> function =
      templ->GetFunction(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"), function)
            .FromJust());
  Local<Value> value = CompileRun("f()");
  CHECK(value->IsUndefined());
  value = CompileRun("new f()");
  CHECK(value->IsFunction());
  CHECK(value == function);
  Local<Value> subclass = CompileRun("var g = class extends f { }; g");
  CHECK(subclass->IsFunction());
  value = CompileRun("new g()");
  CHECK(value->IsFunction());
  CHECK(value == subclass);
  value = CompileRun("Reflect.construct(f, [], Array)");
  CHECK(value->IsFunction());
  CHECK(value ==
        context->Global()
            ->Get(context.local(), v8_str("Array"))
            .ToLocalChecked());
}

THREADED_TEST(ObjectProtoToString) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("MyClass"));

  LocalContext context;

  Local<String> customized_tostring = v8_str("customized toString");

  // Replace Object.prototype.toString
  v8_compile(
      "Object.prototype.toString = function() {"
      "  return 'customized toString';"
      "}")
      ->Run(context.local())
      .ToLocalChecked();

  // Normal ToString call should call replaced Object.prototype.toString
  Local<v8::Object> instance = templ->GetFunction(context.local())
                                   .ToLocalChecked()
                                   ->NewInstance(context.local())
                                   .ToLocalChecked();
  Local<String> value = instance->ToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), customized_tostring).FromJust());

  // ObjectProtoToString should not call replace toString function.
  value = instance->ObjectProtoToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object MyClass]")).FromJust());

  // Check global
  value =
      context->Global()->ObjectProtoToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object Object]")).FromJust());

  // Check ordinary object
  Local<Value> object =
      v8_compile("new Object()")->Run(context.local()).ToLocalChecked();
  value = object.As<v8::Object>()
              ->ObjectProtoToString(context.local())
              .ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object Object]")).FromJust());
}


TEST(ObjectProtoToStringES6) {
  LocalContext context;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("MyClass"));

  Local<String> customized_tostring = v8_str("customized toString");

  // Replace Object.prototype.toString
  CompileRun(
      "Object.prototype.toString = function() {"
      "  return 'customized toString';"
      "}");

  // Normal ToString call should call replaced Object.prototype.toString
  Local<v8::Object> instance = templ->GetFunction(context.local())
                                   .ToLocalChecked()
                                   ->NewInstance(context.local())
                                   .ToLocalChecked();
  Local<String> value = instance->ToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), customized_tostring).FromJust());

  // ObjectProtoToString should not call replace toString function.
  value = instance->ObjectProtoToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object MyClass]")).FromJust());

  // Check global
  value =
      context->Global()->ObjectProtoToString(context.local()).ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object Object]")).FromJust());

  // Check ordinary object
  Local<Value> object = CompileRun("new Object()");
  value = object.As<v8::Object>()
              ->ObjectProtoToString(context.local())
              .ToLocalChecked();
  CHECK(value->IsString() &&
        value->Equals(context.local(), v8_str("[object Object]")).FromJust());

  // Check that ES6 semantics using @@toStringTag work
  Local<v8::Symbol> toStringTag = v8::Symbol::GetToStringTag(isolate);

#define TEST_TOSTRINGTAG(type, tag, expected)                              \
  do {                                                                     \
    object = CompileRun("new " #type "()");                                \
    CHECK(object.As<v8::Object>()                                          \
              ->Set(context.local(), toStringTag, v8_str(#tag))            \
              .FromJust());                                                \
    value = object.As<v8::Object>()                                        \
                ->ObjectProtoToString(context.local())                     \
                .ToLocalChecked();                                         \
    CHECK(value->IsString() &&                                             \
          value->Equals(context.local(), v8_str("[object " #expected "]")) \
              .FromJust());                                                \
  } while (false)

  TEST_TOSTRINGTAG(Array, Object, Object);
  TEST_TOSTRINGTAG(Object, Arguments, Arguments);
  TEST_TOSTRINGTAG(Object, Array, Array);
  TEST_TOSTRINGTAG(Object, Boolean, Boolean);
  TEST_TOSTRINGTAG(Object, Date, Date);
  TEST_TOSTRINGTAG(Object, Error, Error);
  TEST_TOSTRINGTAG(Object, Function, Function);
  TEST_TOSTRINGTAG(Object, Number, Number);
  TEST_TOSTRINGTAG(Object, RegExp, RegExp);
  TEST_TOSTRINGTAG(Object, String, String);
  TEST_TOSTRINGTAG(Object, Foo, Foo);

#undef TEST_TOSTRINGTAG

  Local<v8::RegExp> valueRegExp =
      v8::RegExp::New(context.local(), v8_str("^$"), v8::RegExp::kNone)
          .ToLocalChecked();
  Local<Value> valueNumber = v8_num(123);
  Local<v8::Symbol> valueSymbol = v8_symbol("TestSymbol");
  Local<v8::Function> valueFunction =
      CompileRun("(function fn() {})").As<v8::Function>();
  Local<v8::Object> valueObject = v8::Object::New(isolate);
  Local<v8::Primitive> valueNull = v8::Null(isolate);
  Local<v8::Primitive> valueUndef = v8::Undefined(isolate);

#define TEST_TOSTRINGTAG(type, tagValue, expected)                         \
  do {                                                                     \
    object = CompileRun("new " #type "()");                                \
    CHECK(object.As<v8::Object>()                                          \
              ->Set(context.local(), toStringTag, tagValue)                \
              .FromJust());                                                \
    value = object.As<v8::Object>()                                        \
                ->ObjectProtoToString(context.local())                     \
                .ToLocalChecked();                                         \
    CHECK(value->IsString() &&                                             \
          value->Equals(context.local(), v8_str("[object " #expected "]")) \
              .FromJust());                                                \
  } while (false)

#define TEST_TOSTRINGTAG_TYPES(tagValue)                    \
  TEST_TOSTRINGTAG(Array, tagValue, Array);                 \
  TEST_TOSTRINGTAG(Object, tagValue, Object);               \
  TEST_TOSTRINGTAG(Function, tagValue, Function);           \
  TEST_TOSTRINGTAG(Date, tagValue, Date);                   \
  TEST_TOSTRINGTAG(RegExp, tagValue, RegExp);               \
  TEST_TOSTRINGTAG(Error, tagValue, Error);                 \

  // Test non-String-valued @@toStringTag
  TEST_TOSTRINGTAG_TYPES(valueRegExp);
  TEST_TOSTRINGTAG_TYPES(valueNumber);
  TEST_TOSTRINGTAG_TYPES(valueSymbol);
  TEST_TOSTRINGTAG_TYPES(valueFunction);
  TEST_TOSTRINGTAG_TYPES(valueObject);
  TEST_TOSTRINGTAG_TYPES(valueNull);
  TEST_TOSTRINGTAG_TYPES(valueUndef);

#undef TEST_TOSTRINGTAG
#undef TEST_TOSTRINGTAG_TYPES

  // @@toStringTag getter throws
  Local<Value> obj = v8::Object::New(isolate);
  obj.As<v8::Object>()
      ->SetAccessor(context.local(), toStringTag, ThrowingSymbolAccessorGetter)
      .FromJust();
  {
    TryCatch try_catch(isolate);
    CHECK(obj.As<v8::Object>()->ObjectProtoToString(context.local()).IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  // @@toStringTag getter does not throw
  obj = v8::Object::New(isolate);
  obj.As<v8::Object>()
      ->SetAccessor(context.local(), toStringTag,
                    SymbolAccessorGetterReturnsDefault, nullptr, v8_str("Test"))
      .FromJust();
  {
    TryCatch try_catch(isolate);
    value = obj.As<v8::Object>()
                ->ObjectProtoToString(context.local())
                .ToLocalChecked();
    CHECK(value->IsString() &&
          value->Equals(context.local(), v8_str("[object Test]")).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  // JS @@toStringTag value
  obj = CompileRun("obj = {}; obj[Symbol.toStringTag] = 'Test'; obj");
  {
    TryCatch try_catch(isolate);
    value = obj.As<v8::Object>()
                ->ObjectProtoToString(context.local())
                .ToLocalChecked();
    CHECK(value->IsString() &&
          value->Equals(context.local(), v8_str("[object Test]")).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  // JS @@toStringTag getter throws
  obj = CompileRun(
      "obj = {}; Object.defineProperty(obj, Symbol.toStringTag, {"
      "  get: function() { throw 'Test'; }"
      "}); obj");
  {
    TryCatch try_catch(isolate);
    CHECK(obj.As<v8::Object>()->ObjectProtoToString(context.local()).IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  // JS @@toStringTag getter does not throw
  obj = CompileRun(
      "obj = {}; Object.defineProperty(obj, Symbol.toStringTag, {"
      "  get: function() { return 'Test'; }"
      "}); obj");
  {
    TryCatch try_catch(isolate);
    value = obj.As<v8::Object>()
                ->ObjectProtoToString(context.local())
                .ToLocalChecked();
    CHECK(value->IsString() &&
          value->Equals(context.local(), v8_str("[object Test]")).FromJust());
    CHECK(!try_catch.HasCaught());
  }
}


THREADED_TEST(ObjectGetConstructorName) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext context;
  v8::HandleScope scope(isolate);
  v8_compile(
      "function Parent() {};"
      "function Child() {};"
      "Child.prototype = new Parent();"
      "Child.prototype.constructor = Child;"
      "var outer = { inner: (0, function() { }) };"
      "var p = new Parent();"
      "var c = new Child();"
      "var x = new outer.inner();"
      "var proto = Child.prototype;")
      ->Run(context.local())
      .ToLocalChecked();

  Local<v8::Value> p =
      context->Global()->Get(context.local(), v8_str("p")).ToLocalChecked();
  CHECK(p->IsObject() &&
        p->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("Parent"))
            .FromJust());

  Local<v8::Value> c =
      context->Global()->Get(context.local(), v8_str("c")).ToLocalChecked();
  CHECK(c->IsObject() &&
        c->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("Child"))
            .FromJust());

  Local<v8::Value> x =
      context->Global()->Get(context.local(), v8_str("x")).ToLocalChecked();
  CHECK(x->IsObject() &&
        x->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("outer.inner"))
            .FromJust());

  Local<v8::Value> child_prototype =
      context->Global()->Get(context.local(), v8_str("proto")).ToLocalChecked();
  CHECK(child_prototype->IsObject() &&
        child_prototype->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("Parent"))
            .FromJust());
}


THREADED_TEST(SubclassGetConstructorName) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext context;
  v8::HandleScope scope(isolate);
  v8_compile(
      "\"use strict\";"
      "class Parent {}"
      "class Child extends Parent {}"
      "var p = new Parent();"
      "var c = new Child();")
      ->Run(context.local())
      .ToLocalChecked();

  Local<v8::Value> p =
      context->Global()->Get(context.local(), v8_str("p")).ToLocalChecked();
  CHECK(p->IsObject() &&
        p->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("Parent"))
            .FromJust());

  Local<v8::Value> c =
      context->Global()->Get(context.local(), v8_str("c")).ToLocalChecked();
  CHECK(c->IsObject() &&
        c->ToObject(context.local())
            .ToLocalChecked()
            ->GetConstructorName()
            ->Equals(context.local(), v8_str("Child"))
            .FromJust());
}


bool ApiTestFuzzer::fuzzing_ = false;
v8::base::Semaphore ApiTestFuzzer::all_tests_done_(0);
int ApiTestFuzzer::active_tests_;
int ApiTestFuzzer::tests_being_run_;
int ApiTestFuzzer::current_;


// We are in a callback and want to switch to another thread (if we
// are currently running the thread fuzzing test).
void ApiTestFuzzer::Fuzz() {
  if (!fuzzing_) return;
  ApiTestFuzzer* test = RegisterThreadedTest::nth(current_)->fuzzer_;
  test->ContextSwitch();
}


// Let the next thread go.  Since it is also waiting on the V8 lock it may
// not start immediately.
bool ApiTestFuzzer::NextThread() {
  int test_position = GetNextTestNumber();
  const char* test_name = RegisterThreadedTest::nth(current_)->name();
  if (test_position == current_) {
    if (kLogThreading)
      printf("Stay with %s\n", test_name);
    return false;
  }
  if (kLogThreading) {
    printf("Switch from %s to %s\n",
           test_name,
           RegisterThreadedTest::nth(test_position)->name());
  }
  current_ = test_position;
  RegisterThreadedTest::nth(current_)->fuzzer_->gate_.Signal();
  return true;
}


void ApiTestFuzzer::Run() {
  // When it is our turn...
  gate_.Wait();
  {
    // ... get the V8 lock and start running the test.
    v8::Locker locker(CcTest::isolate());
    CallTest();
  }
  // This test finished.
  active_ = false;
  active_tests_--;
  // If it was the last then signal that fact.
  if (active_tests_ == 0) {
    all_tests_done_.Signal();
  } else {
    // Otherwise select a new test and start that.
    NextThread();
  }
}


static unsigned linear_congruential_generator;


void ApiTestFuzzer::SetUp(PartOfTest part) {
  linear_congruential_generator = i::FLAG_testing_prng_seed;
  fuzzing_ = true;
  int count = RegisterThreadedTest::count();
  int start =  count * part / (LAST_PART + 1);
  int end = (count * (part + 1) / (LAST_PART + 1)) - 1;
  active_tests_ = tests_being_run_ = end - start + 1;
  for (int i = 0; i < tests_being_run_; i++) {
    RegisterThreadedTest::nth(i)->fuzzer_ = new ApiTestFuzzer(i + start);
  }
  for (int i = 0; i < active_tests_; i++) {
    CHECK(RegisterThreadedTest::nth(i)->fuzzer_->Start());
  }
}


static void CallTestNumber(int test_number) {
  (RegisterThreadedTest::nth(test_number)->callback())();
}


void ApiTestFuzzer::RunAllTests() {
  // Set off the first test.
  current_ = -1;
  NextThread();
  // Wait till they are all done.
  all_tests_done_.Wait();
}


int ApiTestFuzzer::GetNextTestNumber() {
  int next_test;
  do {
    next_test = (linear_congruential_generator >> 16) % tests_being_run_;
    linear_congruential_generator *= 1664525u;
    linear_congruential_generator += 1013904223u;
  } while (!RegisterThreadedTest::nth(next_test)->fuzzer_->active_);
  return next_test;
}


void ApiTestFuzzer::ContextSwitch() {
  // If the new thread is the same as the current thread there is nothing to do.
  if (NextThread()) {
    // Now it can start.
    v8::Unlocker unlocker(CcTest::isolate());
    // Wait till someone starts us again.
    gate_.Wait();
    // And we're off.
  }
}


void ApiTestFuzzer::TearDown() {
  fuzzing_ = false;
  for (int i = 0; i < RegisterThreadedTest::count(); i++) {
    ApiTestFuzzer *fuzzer = RegisterThreadedTest::nth(i)->fuzzer_;
    if (fuzzer != nullptr) fuzzer->Join();
  }
}

void ApiTestFuzzer::CallTest() {
  v8::Isolate::Scope scope(CcTest::isolate());
  if (kLogThreading)
    printf("Start test %s #%d\n",
           RegisterThreadedTest::nth(test_number_)->name(), test_number_);
  CallTestNumber(test_number_);
  if (kLogThreading)
    printf("End test %s #%d\n", RegisterThreadedTest::nth(test_number_)->name(),
           test_number_);
}

#define THREADING_TEST(INDEX, NAME)            \
  TEST(Threading##INDEX) {                     \
    ApiTestFuzzer::SetUp(ApiTestFuzzer::NAME); \
    ApiTestFuzzer::RunAllTests();              \
    ApiTestFuzzer::TearDown();                 \
  }

THREADING_TEST(1, FIRST_PART)
THREADING_TEST(2, SECOND_PART)
THREADING_TEST(3, THIRD_PART)
THREADING_TEST(4, FOURTH_PART)
THREADING_TEST(5, FIFTH_PART)
THREADING_TEST(6, SIXTH_PART)
THREADING_TEST(7, SEVENTH_PART)
THREADING_TEST(8, EIGHTH_PART)

#undef THREADING_TEST

static void ThrowInJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  CHECK(v8::Locker::IsLocked(isolate));
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(isolate);
  const char* code = "throw 7;";
  {
    v8::Locker nested_locker(isolate);
    v8::HandleScope scope(isolate);
    v8::Local<Value> exception;
    {
      v8::TryCatch try_catch(isolate);
      v8::Local<Value> value = CompileRun(code);
      CHECK(value.IsEmpty());
      CHECK(try_catch.HasCaught());
      // Make sure to wrap the exception in a new handle because
      // the handle returned from the TryCatch is destroyed
      // when the TryCatch is destroyed.
      exception = Local<Value>::New(isolate, try_catch.Exception());
    }
    args.GetIsolate()->ThrowException(exception);
  }
}


static void ThrowInJSNoCatch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK(v8::Locker::IsLocked(CcTest::isolate()));
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(CcTest::isolate());
  const char* code = "throw 7;";
  {
    v8::Locker nested_locker(CcTest::isolate());
    v8::HandleScope scope(args.GetIsolate());
    v8::Local<Value> value = CompileRun(code);
    CHECK(value.IsEmpty());
    args.GetReturnValue().Set(v8_str("foo"));
  }
}


// These are locking tests that don't need to be run again
// as part of the locking aggregation tests.
TEST(NestedLockers) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Locker locker(isolate);
  CHECK(v8::Locker::IsLocked(isolate));
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate, ThrowInJS);
  Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("throw_in_js"), fun).FromJust());
  Local<Script> script = v8_compile("(function () {"
                                    "  try {"
                                    "    throw_in_js();"
                                    "    return 42;"
                                    "  } catch (e) {"
                                    "    return e * 13;"
                                    "  }"
                                    "})();");
  CHECK_EQ(91, script->Run(env.local())
                   .ToLocalChecked()
                   ->Int32Value(env.local())
                   .FromJust());
}


// These are locking tests that don't need to be run again
// as part of the locking aggregation tests.
TEST(NestedLockersNoTryCatch) {
  v8::Locker locker(CcTest::isolate());
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), ThrowInJSNoCatch);
  Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("throw_in_js"), fun).FromJust());
  Local<Script> script = v8_compile("(function () {"
                                    "  try {"
                                    "    throw_in_js();"
                                    "    return 42;"
                                    "  } catch (e) {"
                                    "    return e * 13;"
                                    "  }"
                                    "})();");
  CHECK_EQ(91, script->Run(env.local())
                   .ToLocalChecked()
                   ->Int32Value(env.local())
                   .FromJust());
}


THREADED_TEST(RecursiveLocking) {
  v8::Locker locker(CcTest::isolate());
  {
    v8::Locker locker2(CcTest::isolate());
    CHECK(v8::Locker::IsLocked(CcTest::isolate()));
  }
}


static void UnlockForAMoment(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(CcTest::isolate());
}


THREADED_TEST(LockUnlockLock) {
  {
    v8::Locker locker(CcTest::isolate());
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), UnlockForAMoment);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("unlock_for_a_moment"), fun)
              .FromJust());
    Local<Script> script = v8_compile("(function () {"
                                      "  unlock_for_a_moment();"
                                      "  return 42;"
                                      "})();");
    CHECK_EQ(42, script->Run(env.local())
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  }
  {
    v8::Locker locker(CcTest::isolate());
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), UnlockForAMoment);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("unlock_for_a_moment"), fun)
              .FromJust());
    Local<Script> script = v8_compile("(function () {"
                                      "  unlock_for_a_moment();"
                                      "  return 42;"
                                      "})();");
    CHECK_EQ(42, script->Run(env.local())
                     .ToLocalChecked()
                     ->Int32Value(env.local())
                     .FromJust());
  }
}


static int GetGlobalObjectsCount() {
  int count = 0;
  i::HeapObjectIterator it(CcTest::heap());
  for (i::HeapObject object = it.Next(); !object.is_null();
       object = it.Next()) {
    if (object.IsJSGlobalObject()) {
      i::JSGlobalObject g = i::JSGlobalObject::cast(object);
      // Skip dummy global object.
      if (g.global_dictionary().NumberOfElements() != 0) {
        count++;
      }
    }
  }
  return count;
}


static void CheckSurvivingGlobalObjectsCount(int expected) {
  // We need to collect all garbage twice to be sure that everything
  // has been collected.  This is because inline caches are cleared in
  // the first garbage collection but some of the maps have already
  // been marked at that point.  Therefore some of the maps are not
  // collected until the second garbage collection.
  CcTest::CollectAllGarbage();
  CcTest::CollectAllGarbage();
  int count = GetGlobalObjectsCount();
  CHECK_EQ(expected, count);
}


TEST(DontLeakGlobalObjects) {
  // Regression test for issues 1139850 and 1174891.

  i::FLAG_expose_gc = true;
  v8::V8::Initialize();

  for (int i = 0; i < 5; i++) {
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      v8_compile("Date")->Run(context.local()).ToLocalChecked();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      v8_compile("/aaa/")->Run(context.local()).ToLocalChecked();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      const char* extension_list[] = { "v8/gc" };
      v8::ExtensionConfiguration extensions(1, extension_list);
      LocalContext context(&extensions);
      v8_compile("gc();")->Run(context.local()).ToLocalChecked();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);
  }
}


TEST(CopyablePersistent) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  i::GlobalHandles* globals =
      reinterpret_cast<i::Isolate*>(isolate)->global_handles();
  size_t initial_handles = globals->handles_count();
  using CopyableObject =
      v8::Persistent<v8::Object, v8::CopyablePersistentTraits<v8::Object>>;
  {
    CopyableObject handle1;
    {
      v8::HandleScope scope(isolate);
      handle1.Reset(isolate, v8::Object::New(isolate));
    }
    CHECK_EQ(initial_handles + 1, globals->handles_count());
    CopyableObject  handle2;
    handle2 = handle1;
    CHECK(handle1 == handle2);
    CHECK_EQ(initial_handles + 2, globals->handles_count());
    CopyableObject handle3(handle2);
    CHECK(handle1 == handle3);
    CHECK_EQ(initial_handles + 3, globals->handles_count());
  }
  // Verify autodispose
  CHECK_EQ(initial_handles, globals->handles_count());
}


static void WeakApiCallback(
    const v8::WeakCallbackInfo<Persistent<v8::Object>>& data) {
  data.GetParameter()->Reset();
  delete data.GetParameter();
}


TEST(WeakCallbackApi) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  i::GlobalHandles* globals =
      reinterpret_cast<i::Isolate*>(isolate)->global_handles();
  size_t initial_handles = globals->handles_count();
  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Object> obj = v8::Object::New(isolate);
    CHECK(
        obj->Set(context.local(), v8_str("key"), v8::Integer::New(isolate, 231))
            .FromJust());
    v8::Persistent<v8::Object>* handle =
        new v8::Persistent<v8::Object>(isolate, obj);
    handle->SetWeak<v8::Persistent<v8::Object>>(
        handle, WeakApiCallback, v8::WeakCallbackType::kParameter);
  }
  CcTest::PreciseCollectAllGarbage();
  // Verify disposed.
  CHECK_EQ(initial_handles, globals->handles_count());
}


v8::Persistent<v8::Object> some_object;
v8::Persistent<v8::Object> bad_handle;


void NewPersistentHandleCallback2(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  v8::HandleScope scope(data.GetIsolate());
  bad_handle.Reset(data.GetIsolate(), some_object);
}


void NewPersistentHandleCallback1(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  data.GetParameter()->Reset();
  data.SetSecondPassCallback(NewPersistentHandleCallback2);
}


THREADED_TEST(NewPersistentHandleFromWeakCallback) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2;
  {
    v8::HandleScope scope(isolate);
    some_object.Reset(isolate, v8::Object::New(isolate));
    handle1.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
  }
  // Note: order is implementation dependent alas: currently
  // global handle nodes are processed by PostGarbageCollectionProcessing
  // in reverse allocation order, so if second allocated handle is deleted,
  // weak callback of the first handle would be able to 'reallocate' it.
  handle1.SetWeak(&handle1, NewPersistentHandleCallback1,
                  v8::WeakCallbackType::kParameter);
  handle2.Reset();
  CcTest::CollectAllGarbage();
}


v8::Persistent<v8::Object> to_be_disposed;


void DisposeAndForceGcCallback2(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  to_be_disposed.Reset();
  CcTest::CollectAllGarbage();
}


void DisposeAndForceGcCallback1(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  data.GetParameter()->Reset();
  data.SetSecondPassCallback(DisposeAndForceGcCallback2);
}


THREADED_TEST(DoNotUseDeletedNodesInSecondLevelGc) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2;
  {
    v8::HandleScope scope(isolate);
    handle1.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
  }
  handle1.SetWeak(&handle1, DisposeAndForceGcCallback1,
                  v8::WeakCallbackType::kParameter);
  to_be_disposed.Reset(isolate, handle2);
  CcTest::CollectAllGarbage();
}

void DisposingCallback(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  data.GetParameter()->Reset();
}

void HandleCreatingCallback2(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  v8::HandleScope scope(data.GetIsolate());
  v8::Global<v8::Object>(data.GetIsolate(), v8::Object::New(data.GetIsolate()));
}


void HandleCreatingCallback1(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  data.GetParameter()->Reset();
  data.SetSecondPassCallback(HandleCreatingCallback2);
}


THREADED_TEST(NoGlobalHandlesOrphaningDueToWeakCallback) {
  v8::Locker locker(CcTest::isolate());
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2, handle3;
  {
    v8::HandleScope scope(isolate);
    handle3.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
    handle1.Reset(isolate, v8::Object::New(isolate));
  }
  handle2.SetWeak(&handle2, DisposingCallback,
                  v8::WeakCallbackType::kParameter);
  handle3.SetWeak(&handle3, HandleCreatingCallback1,
                  v8::WeakCallbackType::kParameter);
  CcTest::CollectAllGarbage();
  EmptyMessageQueues(isolate);
}


THREADED_TEST(CheckForCrossContextObjectLiterals) {
  v8::V8::Initialize();

  const int nof = 2;
  const char* sources[nof] = {
    "try { [ 2, 3, 4 ].forEach(5); } catch(e) { e.toString(); }",
    "Object()"
  };

  for (int i = 0; i < nof; i++) {
    const char* source = sources[i];
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      CompileRun(source);
    }
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      CompileRun(source);
    }
  }
}


static v8::Local<Value> NestedScope(v8::Local<Context> env) {
  v8::EscapableHandleScope inner(env->GetIsolate());
  env->Enter();
  v8::Local<Value> three = v8_num(3);
  v8::Local<Value> value = inner.Escape(three);
  env->Exit();
  return value;
}


THREADED_TEST(NestedHandleScopeAndContexts) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope outer(isolate);
  v8::Local<Context> env = Context::New(isolate);
  env->Enter();
  v8::Local<Value> value = NestedScope(env);
  v8::Local<String> str(value->ToString(env).ToLocalChecked());
  CHECK(!str.IsEmpty());
  env->Exit();
}

static v8::base::HashMap* code_map = nullptr;
static v8::base::HashMap* jitcode_line_info = nullptr;
static int saw_bar = 0;
static int move_events = 0;


static bool FunctionNameIs(const char* expected,
                           const v8::JitCodeEvent* event) {
  // Log lines for functions are of the general form:
  // "LazyCompile:<type><function_name>" or Function:<type><function_name>,
  // where the type is one of "*", "~" or "".
  static const char* kPreamble;
  if (!i::FLAG_lazy) {
    kPreamble = "Function:";
  } else {
    kPreamble = "LazyCompile:";
  }
  static size_t kPreambleLen = strlen(kPreamble);

  if (event->name.len < kPreambleLen ||
      strncmp(kPreamble, event->name.str, kPreambleLen) != 0) {
    return false;
  }

  const char* tail = event->name.str + kPreambleLen;
  size_t tail_len = event->name.len - kPreambleLen;
  size_t expected_len = strlen(expected);
  if (tail_len > 1 && (*tail == '*' || *tail == '~')) {
    --tail_len;
    ++tail;
  }

  // Check for tails like 'bar :1'.
  if (tail_len > expected_len + 2 &&
      tail[expected_len] == ' ' &&
      tail[expected_len + 1] == ':' &&
      tail[expected_len + 2] &&
      !strncmp(tail, expected, expected_len)) {
    return true;
  }

  if (tail_len != expected_len)
    return false;

  return strncmp(tail, expected, expected_len) == 0;
}


static void event_handler(const v8::JitCodeEvent* event) {
  CHECK_NOT_NULL(event);
  CHECK_NOT_NULL(code_map);
  CHECK_NOT_NULL(jitcode_line_info);

  class DummyJitCodeLineInfo {
  };

  switch (event->type) {
    case v8::JitCodeEvent::CODE_ADDED: {
      CHECK_NOT_NULL(event->code_start);
      CHECK_NE(0, static_cast<int>(event->code_len));
      CHECK_NOT_NULL(event->name.str);
      v8::base::HashMap::Entry* entry = code_map->LookupOrInsert(
          event->code_start, i::ComputePointerHash(event->code_start));
      entry->value = reinterpret_cast<void*>(event->code_len);

      if (FunctionNameIs("bar", event)) {
        ++saw_bar;
        }
      }
      break;

    case v8::JitCodeEvent::CODE_MOVED: {
        uint32_t hash = i::ComputePointerHash(event->code_start);
        // We would like to never see code move that we haven't seen before,
        // but the code creation event does not happen until the line endings
        // have been calculated (this is so that we can report the line in the
        // script at which the function source is found, see
        // Compiler::RecordFunctionCompilation) and the line endings
        // calculations can cause a GC, which can move the newly created code
        // before its existence can be logged.
        v8::base::HashMap::Entry* entry =
            code_map->Lookup(event->code_start, hash);
        if (entry != nullptr) {
          ++move_events;

          CHECK_EQ(reinterpret_cast<void*>(event->code_len), entry->value);
          code_map->Remove(event->code_start, hash);

          entry = code_map->LookupOrInsert(
              event->new_code_start,
              i::ComputePointerHash(event->new_code_start));
          entry->value = reinterpret_cast<void*>(event->code_len);
        }
      }
      break;

    case v8::JitCodeEvent::CODE_REMOVED:
      // Object/code removal events are currently not dispatched from the GC.
      UNREACHABLE();

    // For CODE_START_LINE_INFO_RECORDING event, we will create one
    // DummyJitCodeLineInfo data structure pointed by event->user_dat. We
    // record it in jitcode_line_info.
    case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING: {
        DummyJitCodeLineInfo* line_info = new DummyJitCodeLineInfo();
        v8::JitCodeEvent* temp_event = const_cast<v8::JitCodeEvent*>(event);
        temp_event->user_data = line_info;
        v8::base::HashMap::Entry* entry = jitcode_line_info->LookupOrInsert(
            line_info, i::ComputePointerHash(line_info));
        entry->value = reinterpret_cast<void*>(line_info);
      }
      break;
    // For these two events, we will check whether the event->user_data
    // data structure is created before during CODE_START_LINE_INFO_RECORDING
    // event. And delete it in CODE_END_LINE_INFO_RECORDING event handling.
    case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING: {
      CHECK_NOT_NULL(event->user_data);
      uint32_t hash = i::ComputePointerHash(event->user_data);
      v8::base::HashMap::Entry* entry =
          jitcode_line_info->Lookup(event->user_data, hash);
      CHECK_NOT_NULL(entry);
      delete reinterpret_cast<DummyJitCodeLineInfo*>(event->user_data);
      }
      break;

    case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO: {
      CHECK_NOT_NULL(event->user_data);
      uint32_t hash = i::ComputePointerHash(event->user_data);
      v8::base::HashMap::Entry* entry =
          jitcode_line_info->Lookup(event->user_data, hash);
      CHECK_NOT_NULL(entry);
      }
      break;

    default:
      // Impossible event.
      UNREACHABLE();
  }
}


UNINITIALIZED_TEST(SetJitCodeEventHandler) {
  i::FLAG_stress_compaction = true;
  i::FLAG_incremental_marking = false;
  if (i::FLAG_never_compact) return;
  const char* script =
      "function bar() {"
      "  var sum = 0;"
      "  for (i = 0; i < 10; ++i)"
      "    sum = foo(i);"
      "  return sum;"
      "}"
      "function foo(i) { return i; };"
      "bar();";

  // Run this test in a new isolate to make sure we don't
  // have remnants of state from other code.
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  isolate->Enter();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Heap* heap = i_isolate->heap();

  // Start with a clean slate.
  heap->CollectAllAvailableGarbage(i::GarbageCollectionReason::kTesting);

  {
    v8::HandleScope scope(isolate);
    v8::base::HashMap code;
    code_map = &code;

    v8::base::HashMap lineinfo;
    jitcode_line_info = &lineinfo;

    saw_bar = 0;
    move_events = 0;

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, event_handler);

    // Generate new code objects sparsely distributed across several
    // different fragmented code-space pages.
    const int kIterations = 10;
    for (int i = 0; i < kIterations; ++i) {
      LocalContext env(isolate);
      i::AlwaysAllocateScope always_allocate(i_isolate);
      CompileRun(script);

      // Keep a strong reference to the code object in the handle scope.
      i::Handle<i::JSFunction> bar(i::Handle<i::JSFunction>::cast(
          v8::Utils::OpenHandle(*env->Global()
                                     ->Get(env.local(), v8_str("bar"))
                                     .ToLocalChecked())));
      i::Handle<i::JSFunction> foo(i::Handle<i::JSFunction>::cast(
          v8::Utils::OpenHandle(*env->Global()
                                     ->Get(env.local(), v8_str("foo"))
                                     .ToLocalChecked())));

      i::PagedSpace* foo_owning_space = reinterpret_cast<i::PagedSpace*>(
          i::Page::FromHeapObject(foo->abstract_code())->owner());
      i::PagedSpace* bar_owning_space = reinterpret_cast<i::PagedSpace*>(
          i::Page::FromHeapObject(bar->abstract_code())->owner());
      CHECK_EQ(foo_owning_space, bar_owning_space);
      i::heap::SimulateFullSpace(foo_owning_space);

      // Clear the compilation cache to get more wastage.
      reinterpret_cast<i::Isolate*>(isolate)->compilation_cache()->Clear();
    }

    // Force code movement.
    heap->CollectAllAvailableGarbage(i::GarbageCollectionReason::kTesting);

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);

    CHECK_LE(kIterations, saw_bar);
    CHECK_LT(0, move_events);

    code_map = nullptr;
    jitcode_line_info = nullptr;
  }

  isolate->Exit();
  isolate->Dispose();

  // Do this in a new isolate.
  isolate = v8::Isolate::New(create_params);
  isolate->Enter();

  // Verify that we get callbacks for existing code objects when we
  // request enumeration of existing code.
  {
    v8::HandleScope scope(isolate);
    LocalContext env(isolate);
    CompileRun(script);

    // Now get code through initial iteration.
    v8::base::HashMap code;
    code_map = &code;

    v8::base::HashMap lineinfo;
    jitcode_line_info = &lineinfo;

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventEnumExisting,
                                    event_handler);
    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);

    jitcode_line_info = nullptr;
    // We expect that we got some events. Note that if we could get code removal
    // notifications, we could compare two collections, one created by listening
    // from the time of creation of an isolate, and the other by subscribing
    // with EnumExisting.
    CHECK_LT(0u, code.occupancy());

    code_map = nullptr;
  }

  isolate->Exit();
  isolate->Dispose();
}

TEST(ExternalAllocatedMemory) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope outer(isolate);
  v8::Local<Context> env(Context::New(isolate));
  CHECK(!env.IsEmpty());
  const int64_t kSize = 1024*1024;
  int64_t baseline = isolate->AdjustAmountOfExternalAllocatedMemory(0);
  CHECK_EQ(baseline + kSize,
           isolate->AdjustAmountOfExternalAllocatedMemory(kSize));
  CHECK_EQ(baseline,
           isolate->AdjustAmountOfExternalAllocatedMemory(-kSize));
  const int64_t kTriggerGCSize =
      CcTest::i_isolate()->heap()->external_memory_hard_limit() + 1;
  CHECK_EQ(baseline + kTriggerGCSize,
           isolate->AdjustAmountOfExternalAllocatedMemory(kTriggerGCSize));
  CHECK_EQ(baseline,
           isolate->AdjustAmountOfExternalAllocatedMemory(-kTriggerGCSize));
}


TEST(Regress51719) {
  i::FLAG_incremental_marking = false;
  CcTest::InitializeVM();

  const int64_t kTriggerGCSize =
      CcTest::i_isolate()->heap()->external_memory_hard_limit() + 1;
  v8::Isolate* isolate = CcTest::isolate();
  isolate->AdjustAmountOfExternalAllocatedMemory(kTriggerGCSize);
}

// Regression test for issue 54, object templates with embedder fields
// but no accessors or interceptors did not get their embedder field
// count set on instances.
THREADED_TEST(Regress54) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope outer(isolate);
  static v8::Persistent<v8::ObjectTemplate> templ;
  if (templ.IsEmpty()) {
    v8::EscapableHandleScope inner(isolate);
    v8::Local<v8::ObjectTemplate> local = v8::ObjectTemplate::New(isolate);
    local->SetInternalFieldCount(1);
    templ.Reset(isolate, inner.Escape(local));
  }
  v8::Local<v8::Object> result =
      v8::Local<v8::ObjectTemplate>::New(isolate, templ)
          ->NewInstance(context.local())
          .ToLocalChecked();
  CHECK_EQ(1, result->InternalFieldCount());
}


// If part of the threaded tests, this test makes ThreadingTest fail
// on mac.
TEST(CatchStackOverflow) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  v8::Local<v8::Value> result = CompileRun(
      "function f() {"
      "  return f();"
      "}"
      ""
      "f();");
  CHECK(result.IsEmpty());
}


static void CheckTryCatchSourceInfo(v8::Local<v8::Script> script,
                                    const char* resource_name,
                                    int line_offset) {
  v8::HandleScope scope(CcTest::isolate());
  v8::TryCatch try_catch(CcTest::isolate());
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  CHECK(script->Run(context).IsEmpty());
  CHECK(try_catch.HasCaught());
  v8::Local<v8::Message> message = try_catch.Message();
  CHECK(!message.IsEmpty());
  CHECK_EQ(10 + line_offset, message->GetLineNumber(context).FromJust());
  CHECK_EQ(91, message->GetStartPosition());
  CHECK_EQ(92, message->GetEndPosition());
  CHECK_EQ(2, message->GetStartColumn(context).FromJust());
  CHECK_EQ(3, message->GetEndColumn(context).FromJust());
  v8::String::Utf8Value line(CcTest::isolate(),
                             message->GetSourceLine(context).ToLocalChecked());
  CHECK_EQ(0, strcmp("  throw 'nirk';", *line));
  v8::String::Utf8Value name(CcTest::isolate(),
                             message->GetScriptOrigin().ResourceName());
  CHECK_EQ(0, strcmp(resource_name, *name));
}


THREADED_TEST(TryCatchSourceInfo) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<v8::String> source = v8_str(
      "function Foo() {\n"
      "  return Bar();\n"
      "}\n"
      "\n"
      "function Bar() {\n"
      "  return Baz();\n"
      "}\n"
      "\n"
      "function Baz() {\n"
      "  throw 'nirk';\n"
      "}\n"
      "\n"
      "Foo();\n");

  const char* resource_name;
  v8::Local<v8::Script> script;
  resource_name = "test.js";
  script = CompileWithOrigin(source, resource_name, false);
  CheckTryCatchSourceInfo(script, resource_name, 0);

  resource_name = "test1.js";
  v8::ScriptOrigin origin1(v8_str(resource_name));
  script =
      v8::Script::Compile(context.local(), source, &origin1).ToLocalChecked();
  CheckTryCatchSourceInfo(script, resource_name, 0);

  resource_name = "test2.js";
  v8::ScriptOrigin origin2(v8_str(resource_name),
                           v8::Integer::New(context->GetIsolate(), 7));
  script =
      v8::Script::Compile(context.local(), source, &origin2).ToLocalChecked();
  CheckTryCatchSourceInfo(script, resource_name, 7);
}


THREADED_TEST(TryCatchSourceInfoForEOSError) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch(context->GetIsolate());
  CHECK(v8::Script::Compile(context.local(), v8_str("!\n")).IsEmpty());
  CHECK(try_catch.HasCaught());
  v8::Local<v8::Message> message = try_catch.Message();
  CHECK_EQ(2, message->GetLineNumber(context.local()).FromJust());
  CHECK_EQ(0, message->GetStartColumn(context.local()).FromJust());
}


THREADED_TEST(CompilationCache) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<v8::String> source0 = v8_str("1234");
  v8::Local<v8::String> source1 = v8_str("1234");
  v8::Local<v8::Script> script0 = CompileWithOrigin(source0, "test.js", false);
  v8::Local<v8::Script> script1 = CompileWithOrigin(source1, "test.js", false);
  v8::Local<v8::Script> script2 = v8::Script::Compile(context.local(), source0)
                                      .ToLocalChecked();  // different origin
  CHECK_EQ(1234, script0->Run(context.local())
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  CHECK_EQ(1234, script1->Run(context.local())
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  CHECK_EQ(1234, script2->Run(context.local())
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
}


static void FunctionNameCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(42));
}


THREADED_TEST(CallbackFunctionName) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> t = ObjectTemplate::New(isolate);
  t->Set(v8_str("asdf"),
         v8::FunctionTemplate::New(isolate, FunctionNameCallback));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  t->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  v8::Local<v8::Value> value = CompileRun("obj.asdf.name");
  CHECK(value->IsString());
  v8::String::Utf8Value name(isolate, value);
  CHECK_EQ(0, strcmp("asdf", *name));
}


THREADED_TEST(DateAccess) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<v8::Value> date =
      v8::Date::New(context.local(), 1224744689038.0).ToLocalChecked();
  CHECK(date->IsDate());
  CHECK_EQ(1224744689038.0, date.As<v8::Date>()->ValueOf());
}

void CheckIsSymbolAt(v8::Isolate* isolate, v8::Local<v8::Array> properties,
                     unsigned index, const char* name) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> value =
      properties->Get(context, v8::Integer::New(isolate, index))
          .ToLocalChecked();
  CHECK(value->IsSymbol());
  v8::String::Utf8Value symbol_name(isolate,
                                    Local<Symbol>::Cast(value)->Name());
  if (strcmp(name, *symbol_name) != 0) {
    FATAL("properties[%u] was Symbol('%s') instead of Symbol('%s').", index,
          name, *symbol_name);
  }
}

void CheckStringArray(v8::Isolate* isolate, v8::Local<v8::Array> properties,
                      unsigned length, const char* names[]) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  CHECK_EQ(length, properties->Length());
  for (unsigned i = 0; i < length; i++) {
    v8::Local<v8::Value> value =
        properties->Get(context, v8::Integer::New(isolate, i)).ToLocalChecked();
    if (names[i] == nullptr) {
      DCHECK(value->IsSymbol());
    } else {
      v8::String::Utf8Value elm(isolate, value);
      if (strcmp(names[i], *elm) != 0) {
        FATAL("properties[%u] was '%s' instead of '%s'.", i, *elm, names[i]);
      }
    }
  }
}

void CheckProperties(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     unsigned length, const char* names[]) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> obj = val.As<v8::Object>();
  v8::Local<v8::Array> props = obj->GetPropertyNames(context).ToLocalChecked();
  CheckStringArray(isolate, props, length, names);
}


void CheckOwnProperties(v8::Isolate* isolate, v8::Local<v8::Value> val,
                        unsigned elmc, const char* elmv[]) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> obj = val.As<v8::Object>();
  v8::Local<v8::Array> props =
      obj->GetOwnPropertyNames(context).ToLocalChecked();
  CHECK_EQ(elmc, props->Length());
  for (unsigned i = 0; i < elmc; i++) {
    v8::String::Utf8Value elm(
        isolate,
        props->Get(context, v8::Integer::New(isolate, i)).ToLocalChecked());
    CHECK_EQ(0, strcmp(elmv[i], *elm));
  }
}


THREADED_TEST(PropertyEnumeration) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> obj = CompileRun(
      "var result = [];"
      "result[0] = {};"
      "result[1] = {a: 1, b: 2};"
      "result[2] = [1, 2, 3];"
      "var proto = {x: 1, y: 2, z: 3};"
      "var x = { __proto__: proto, w: 0, z: 1 };"
      "result[3] = x;"
      "result[4] = {21350:1};"
      "x = Object.create(null);"
      "x.a = 1; x[12345678] = 1;"
      "result[5] = x;"
      "result;");
  v8::Local<v8::Array> elms = obj.As<v8::Array>();
  CHECK_EQ(6u, elms->Length());
  int elmc0 = 0;
  const char** elmv0 = nullptr;
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 0)).ToLocalChecked(),
      elmc0, elmv0);
  CheckOwnProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 0)).ToLocalChecked(),
      elmc0, elmv0);
  int elmc1 = 2;
  const char* elmv1[] = {"a", "b"};
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 1)).ToLocalChecked(),
      elmc1, elmv1);
  CheckOwnProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 1)).ToLocalChecked(),
      elmc1, elmv1);
  int elmc2 = 3;
  const char* elmv2[] = {"0", "1", "2"};
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 2)).ToLocalChecked(),
      elmc2, elmv2);
  CheckOwnProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 2)).ToLocalChecked(),
      elmc2, elmv2);
  int elmc3 = 4;
  const char* elmv3[] = {"w", "z", "x", "y"};
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 3)).ToLocalChecked(),
      elmc3, elmv3);
  int elmc4 = 2;
  const char* elmv4[] = {"w", "z"};
  CheckOwnProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 3)).ToLocalChecked(),
      elmc4, elmv4);
  // Dictionary elements.
  int elmc5 = 1;
  const char* elmv5[] = {"21350"};
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 4)).ToLocalChecked(),
      elmc5, elmv5);
  // Dictionary properties.
  int elmc6 = 2;
  const char* elmv6[] = {"12345678", "a"};
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 5)).ToLocalChecked(),
      elmc6, elmv6);
}


THREADED_TEST(PropertyEnumeration2) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> obj = CompileRun(
      "var result = [];"
      "result[0] = {};"
      "result[1] = {a: 1, b: 2};"
      "result[2] = [1, 2, 3];"
      "var proto = {x: 1, y: 2, z: 3};"
      "var x = { __proto__: proto, w: 0, z: 1 };"
      "result[3] = x;"
      "result;");
  v8::Local<v8::Array> elms = obj.As<v8::Array>();
  CHECK_EQ(4u, elms->Length());
  int elmc0 = 0;
  const char** elmv0 = nullptr;
  CheckProperties(
      isolate,
      elms->Get(context.local(), v8::Integer::New(isolate, 0)).ToLocalChecked(),
      elmc0, elmv0);

  v8::Local<v8::Value> val =
      elms->Get(context.local(), v8::Integer::New(isolate, 0)).ToLocalChecked();
  v8::Local<v8::Array> props =
      val.As<v8::Object>()->GetPropertyNames(context.local()).ToLocalChecked();
  CHECK_EQ(0u, props->Length());
  for (uint32_t i = 0; i < props->Length(); i++) {
    printf("p[%u]\n", i);
  }
}

THREADED_TEST(GetPropertyNames) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> result = CompileRun(
      "var result = {0: 0, 1: 1, a: 2, b: 3};"
      "result[2**32] = '4294967296';"
      "result[2**32-1] = '4294967295';"
      "result[2**32-2] = '4294967294';"
      "result[Symbol('symbol')] = true;"
      "result.__proto__ = {__proto__:null, 2: 4, 3: 5, c: 6, d: 7};"
      "result;");
  v8::Local<v8::Object> object = result.As<v8::Object>();
  v8::PropertyFilter default_filter =
      static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS);
  v8::PropertyFilter include_symbols_filter = v8::ONLY_ENUMERABLE;

  v8::Local<v8::Array> properties =
      object->GetPropertyNames(context.local()).ToLocalChecked();
  const char* expected_properties1[] = {"0", "1",          "4294967294", "a",
                                        "b", "4294967296", "4294967295", "2",
                                        "3", "c",          "d"};
  CheckStringArray(isolate, properties, 11, expected_properties1);

  properties =
      object
          ->GetPropertyNames(context.local(),
                             v8::KeyCollectionMode::kIncludePrototypes,
                             default_filter, v8::IndexFilter::kIncludeIndices)
          .ToLocalChecked();
  CheckStringArray(isolate, properties, 11, expected_properties1);

  properties = object
                   ->GetPropertyNames(context.local(),
                                      v8::KeyCollectionMode::kIncludePrototypes,
                                      include_symbols_filter,
                                      v8::IndexFilter::kIncludeIndices)
                   .ToLocalChecked();
  const char* expected_properties1_1[] = {
      "0",          "1",     "4294967294", "a", "b", "4294967296",
      "4294967295", nullptr, "2",          "3", "c", "d"};
  CheckStringArray(isolate, properties, 12, expected_properties1_1);
  CheckIsSymbolAt(isolate, properties, 7, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(),
                             v8::KeyCollectionMode::kIncludePrototypes,
                             default_filter, v8::IndexFilter::kSkipIndices)
          .ToLocalChecked();
  const char* expected_properties2[] = {"a",          "b", "4294967296",
                                        "4294967295", "c", "d"};
  CheckStringArray(isolate, properties, 6, expected_properties2);

  properties = object
                   ->GetPropertyNames(context.local(),
                                      v8::KeyCollectionMode::kIncludePrototypes,
                                      include_symbols_filter,
                                      v8::IndexFilter::kSkipIndices)
                   .ToLocalChecked();
  const char* expected_properties2_1[] = {
      "a", "b", "4294967296", "4294967295", nullptr, "c", "d"};
  CheckStringArray(isolate, properties, 7, expected_properties2_1);
  CheckIsSymbolAt(isolate, properties, 4, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(), v8::KeyCollectionMode::kOwnOnly,
                             default_filter, v8::IndexFilter::kIncludeIndices)
          .ToLocalChecked();
  const char* expected_properties3[] = {
      "0", "1", "4294967294", "a", "b", "4294967296", "4294967295",
  };
  CheckStringArray(isolate, properties, 7, expected_properties3);

  properties = object
                   ->GetPropertyNames(
                       context.local(), v8::KeyCollectionMode::kOwnOnly,
                       include_symbols_filter, v8::IndexFilter::kIncludeIndices)
                   .ToLocalChecked();
  const char* expected_properties3_1[] = {
      "0", "1", "4294967294", "a", "b", "4294967296", "4294967295", nullptr};
  CheckStringArray(isolate, properties, 8, expected_properties3_1);
  CheckIsSymbolAt(isolate, properties, 7, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(), v8::KeyCollectionMode::kOwnOnly,
                             default_filter, v8::IndexFilter::kSkipIndices)
          .ToLocalChecked();
  const char* expected_properties4[] = {"a", "b", "4294967296", "4294967295"};
  CheckStringArray(isolate, properties, 4, expected_properties4);

  properties = object
                   ->GetPropertyNames(
                       context.local(), v8::KeyCollectionMode::kOwnOnly,
                       include_symbols_filter, v8::IndexFilter::kSkipIndices)
                   .ToLocalChecked();
  const char* expected_properties4_1[] = {"a", "b", "4294967296", "4294967295",
                                          nullptr};
  CheckStringArray(isolate, properties, 5, expected_properties4_1);
  CheckIsSymbolAt(isolate, properties, 4, "symbol");
}

THREADED_TEST(ProxyGetPropertyNames) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> result = CompileRun(
      "var target = {0: 0, 1: 1, a: 2, b: 3};"
      "target[2**32] = '4294967296';"
      "target[2**32-1] = '4294967295';"
      "target[2**32-2] = '4294967294';"
      "target[Symbol('symbol')] = true;"
      "target.__proto__ = {__proto__:null, 2: 4, 3: 5, c: 6, d: 7};"
      "var result = new Proxy(target, {});"
      "result;");
  v8::Local<v8::Object> object = result.As<v8::Object>();
  v8::PropertyFilter default_filter =
      static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS);
  v8::PropertyFilter include_symbols_filter = v8::ONLY_ENUMERABLE;

  v8::Local<v8::Array> properties =
      object->GetPropertyNames(context.local()).ToLocalChecked();
  const char* expected_properties1[] = {"0", "1",          "4294967294", "a",
                                        "b", "4294967296", "4294967295", "2",
                                        "3", "c",          "d"};
  CheckStringArray(isolate, properties, 11, expected_properties1);

  properties =
      object
          ->GetPropertyNames(context.local(),
                             v8::KeyCollectionMode::kIncludePrototypes,
                             default_filter, v8::IndexFilter::kIncludeIndices)
          .ToLocalChecked();
  CheckStringArray(isolate, properties, 11, expected_properties1);

  properties = object
                   ->GetPropertyNames(context.local(),
                                      v8::KeyCollectionMode::kIncludePrototypes,
                                      include_symbols_filter,
                                      v8::IndexFilter::kIncludeIndices)
                   .ToLocalChecked();
  const char* expected_properties1_1[] = {
      "0",          "1",     "4294967294", "a", "b", "4294967296",
      "4294967295", nullptr, "2",          "3", "c", "d"};
  CheckStringArray(isolate, properties, 12, expected_properties1_1);
  CheckIsSymbolAt(isolate, properties, 7, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(),
                             v8::KeyCollectionMode::kIncludePrototypes,
                             default_filter, v8::IndexFilter::kSkipIndices)
          .ToLocalChecked();
  const char* expected_properties2[] = {"a",          "b", "4294967296",
                                        "4294967295", "c", "d"};
  CheckStringArray(isolate, properties, 6, expected_properties2);

  properties = object
                   ->GetPropertyNames(context.local(),
                                      v8::KeyCollectionMode::kIncludePrototypes,
                                      include_symbols_filter,
                                      v8::IndexFilter::kSkipIndices)
                   .ToLocalChecked();
  const char* expected_properties2_1[] = {
      "a", "b", "4294967296", "4294967295", nullptr, "c", "d"};
  CheckStringArray(isolate, properties, 7, expected_properties2_1);
  CheckIsSymbolAt(isolate, properties, 4, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(), v8::KeyCollectionMode::kOwnOnly,
                             default_filter, v8::IndexFilter::kIncludeIndices)
          .ToLocalChecked();
  const char* expected_properties3[] = {"0", "1",          "4294967294", "a",
                                        "b", "4294967296", "4294967295"};
  CheckStringArray(isolate, properties, 7, expected_properties3);

  properties = object
                   ->GetPropertyNames(
                       context.local(), v8::KeyCollectionMode::kOwnOnly,
                       include_symbols_filter, v8::IndexFilter::kIncludeIndices)
                   .ToLocalChecked();
  const char* expected_properties3_1[] = {
      "0", "1", "4294967294", "a", "b", "4294967296", "4294967295", nullptr};
  CheckStringArray(isolate, properties, 8, expected_properties3_1);
  CheckIsSymbolAt(isolate, properties, 7, "symbol");

  properties =
      object
          ->GetPropertyNames(context.local(), v8::KeyCollectionMode::kOwnOnly,
                             default_filter, v8::IndexFilter::kSkipIndices)
          .ToLocalChecked();
  const char* expected_properties4[] = {"a", "b", "4294967296", "4294967295"};
  CheckStringArray(isolate, properties, 4, expected_properties4);

  properties = object
                   ->GetPropertyNames(
                       context.local(), v8::KeyCollectionMode::kOwnOnly,
                       include_symbols_filter, v8::IndexFilter::kSkipIndices)
                   .ToLocalChecked();
  const char* expected_properties4_1[] = {"a", "b", "4294967296", "4294967295",
                                          nullptr};
  CheckStringArray(isolate, properties, 5, expected_properties4_1);
  CheckIsSymbolAt(isolate, properties, 4, "symbol");
}

THREADED_TEST(AccessChecksReenabledCorrectly) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallback(AccessAlwaysBlocked);
  templ->Set(v8_str("a"), v8_str("a"));
  // Add more than 8 (see kMaxFastProperties) properties
  // so that the constructor will force copying map.
  // Cannot sprintf, gcc complains unsafety.
  char buf[4];
  for (char i = '0'; i <= '9' ; i++) {
    buf[0] = i;
    for (char j = '0'; j <= '9'; j++) {
      buf[1] = j;
      for (char k = '0'; k <= '9'; k++) {
        buf[2] = k;
        buf[3] = 0;
        templ->Set(v8_str(buf), v8::Number::New(isolate, k));
      }
    }
  }

  Local<v8::Object> instance_1 =
      templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj_1"), instance_1)
            .FromJust());

  Local<Value> value_1 = CompileRun("obj_1.a");
  CHECK(value_1.IsEmpty());

  Local<v8::Object> instance_2 =
      templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj_2"), instance_2)
            .FromJust());

  Local<Value> value_2 = CompileRun("obj_2.a");
  CHECK(value_2.IsEmpty());
}


// This tests that we do not allow dictionary load/call inline caches
// to use functions that have not yet been compiled.  The potential
// problem of loading a function that has not yet been compiled can
// arise because we share code between contexts via the compilation
// cache.
THREADED_TEST(DictionaryICLoadedFunction) {
  v8::HandleScope scope(CcTest::isolate());
  // Test LoadIC.
  for (int i = 0; i < 2; i++) {
    LocalContext context;
    CHECK(context->Global()
              ->Set(context.local(), v8_str("tmp"), v8::True(CcTest::isolate()))
              .FromJust());
    context->Global()->Delete(context.local(), v8_str("tmp")).FromJust();
    CompileRun("for (var j = 0; j < 10; j++) new RegExp('');");
  }
  // Test CallIC.
  for (int i = 0; i < 2; i++) {
    LocalContext context;
    CHECK(context->Global()
              ->Set(context.local(), v8_str("tmp"), v8::True(CcTest::isolate()))
              .FromJust());
    context->Global()->Delete(context.local(), v8_str("tmp")).FromJust();
    CompileRun("for (var j = 0; j < 10; j++) RegExp('')");
  }
}


// Test that cross-context new calls use the context of the callee to
// create the new JavaScript object.
THREADED_TEST(CrossContextNew) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<Context> context0 = Context::New(isolate);
  v8::Local<Context> context1 = Context::New(isolate);

  // Allow cross-domain access.
  Local<String> token = v8_str("<security token>");
  context0->SetSecurityToken(token);
  context1->SetSecurityToken(token);

  // Set an 'x' property on the Object prototype and define a
  // constructor function in context0.
  context0->Enter();
  CompileRun("Object.prototype.x = 42; function C() {};");
  context0->Exit();

  // Call the constructor function from context0 and check that the
  // result has the 'x' property.
  context1->Enter();
  CHECK(context1->Global()
            ->Set(context1, v8_str("other"), context0->Global())
            .FromJust());
  Local<Value> value = CompileRun("var instance = new other.C(); instance.x");
  CHECK(value->IsInt32());
  CHECK_EQ(42, value->Int32Value(context1).FromJust());
  context1->Exit();
}


// Verify that we can clone an object
TEST(ObjectClone) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  const char* sample =
    "var rv = {};"      \
    "rv.alpha = 'hello';" \
    "rv.beta = 123;"     \
    "rv;";

  // Create an object, verify basics.
  Local<Value> val = CompileRun(sample);
  CHECK(val->IsObject());
  Local<v8::Object> obj = val.As<v8::Object>();
  obj->Set(env.local(), v8_str("gamma"), v8_str("cloneme")).FromJust();

  CHECK(v8_str("hello")
            ->Equals(env.local(),
                     obj->Get(env.local(), v8_str("alpha")).ToLocalChecked())
            .FromJust());
  CHECK(v8::Integer::New(isolate, 123)
            ->Equals(env.local(),
                     obj->Get(env.local(), v8_str("beta")).ToLocalChecked())
            .FromJust());
  CHECK(v8_str("cloneme")
            ->Equals(env.local(),
                     obj->Get(env.local(), v8_str("gamma")).ToLocalChecked())
            .FromJust());

  // Clone it.
  Local<v8::Object> clone = obj->Clone();
  CHECK(v8_str("hello")
            ->Equals(env.local(),
                     clone->Get(env.local(), v8_str("alpha")).ToLocalChecked())
            .FromJust());
  CHECK(v8::Integer::New(isolate, 123)
            ->Equals(env.local(),
                     clone->Get(env.local(), v8_str("beta")).ToLocalChecked())
            .FromJust());
  CHECK(v8_str("cloneme")
            ->Equals(env.local(),
                     clone->Get(env.local(), v8_str("gamma")).ToLocalChecked())
            .FromJust());

  // Set a property on the clone, verify each object.
  CHECK(clone->Set(env.local(), v8_str("beta"), v8::Integer::New(isolate, 456))
            .FromJust());
  CHECK(v8::Integer::New(isolate, 123)
            ->Equals(env.local(),
                     obj->Get(env.local(), v8_str("beta")).ToLocalChecked())
            .FromJust());
  CHECK(v8::Integer::New(isolate, 456)
            ->Equals(env.local(),
                     clone->Get(env.local(), v8_str("beta")).ToLocalChecked())
            .FromJust());
}


class OneByteVectorResource : public v8::String::ExternalOneByteStringResource {
 public:
  explicit OneByteVectorResource(i::Vector<const char> vector)
      : data_(vector) {}
  ~OneByteVectorResource() override = default;
  size_t length() const override { return data_.length(); }
  const char* data() const override { return data_.begin(); }
  void Dispose() override {}

 private:
  i::Vector<const char> data_;
};


class UC16VectorResource : public v8::String::ExternalStringResource {
 public:
  explicit UC16VectorResource(i::Vector<const i::uc16> vector)
      : data_(vector) {}
  ~UC16VectorResource() override = default;
  size_t length() const override { return data_.length(); }
  const i::uc16* data() const override { return data_.begin(); }
  void Dispose() override {}

 private:
  i::Vector<const i::uc16> data_;
};

static void MorphAString(i::String string,
                         OneByteVectorResource* one_byte_resource,
                         UC16VectorResource* uc16_resource) {
  i::Isolate* isolate = CcTest::i_isolate();
  CHECK(i::StringShape(string).IsExternal());
  i::ReadOnlyRoots roots(CcTest::heap());
  if (string.IsOneByteRepresentation()) {
    // Check old map is not internalized or long.
    CHECK(string.map() == roots.external_one_byte_string_map());
    // Morph external string to be TwoByte string.
    string.set_map(roots.external_string_map());
    i::ExternalTwoByteString morphed = i::ExternalTwoByteString::cast(string);
    CcTest::heap()->UpdateExternalString(morphed, string.length(), 0);
    morphed.SetResource(isolate, uc16_resource);
  } else {
    // Check old map is not internalized or long.
    CHECK(string.map() == roots.external_string_map());
    // Morph external string to be one-byte string.
    string.set_map(roots.external_one_byte_string_map());
    i::ExternalOneByteString morphed = i::ExternalOneByteString::cast(string);
    CcTest::heap()->UpdateExternalString(morphed, string.length(), 0);
    morphed.SetResource(isolate, one_byte_resource);
  }
}

// Test that we can still flatten a string if the components it is built up
// from have been turned into 16 bit strings in the mean time.
THREADED_TEST(MorphCompositeStringTest) {
  char utf_buffer[129];
  const char* c_string = "Now is the time for all good men"
                         " to come to the aid of the party";
  uint16_t* two_byte_string = AsciiToTwoByteString(c_string);
  {
    LocalContext env;
    i::Factory* factory = CcTest::i_isolate()->factory();
    v8::Isolate* isolate = env->GetIsolate();
    i::Isolate* i_isolate = CcTest::i_isolate();
    v8::HandleScope scope(isolate);
    OneByteVectorResource one_byte_resource(
        i::Vector<const char>(c_string, strlen(c_string)));
    UC16VectorResource uc16_resource(
        i::Vector<const uint16_t>(two_byte_string, strlen(c_string)));

    Local<String> lhs(v8::Utils::ToLocal(
        factory->NewExternalStringFromOneByte(&one_byte_resource)
            .ToHandleChecked()));
    Local<String> rhs(v8::Utils::ToLocal(
        factory->NewExternalStringFromOneByte(&one_byte_resource)
            .ToHandleChecked()));

    CHECK(env->Global()->Set(env.local(), v8_str("lhs"), lhs).FromJust());
    CHECK(env->Global()->Set(env.local(), v8_str("rhs"), rhs).FromJust());

    CompileRun(
        "var cons = lhs + rhs;"
        "var slice = lhs.substring(1, lhs.length - 1);"
        "var slice_on_cons = (lhs + rhs).substring(1, lhs.length *2 - 1);");

    CHECK(lhs->IsOneByte());
    CHECK(rhs->IsOneByte());

    i::String ilhs = *v8::Utils::OpenHandle(*lhs);
    i::String irhs = *v8::Utils::OpenHandle(*rhs);
    MorphAString(ilhs, &one_byte_resource, &uc16_resource);
    MorphAString(irhs, &one_byte_resource, &uc16_resource);

    // This should UTF-8 without flattening, since everything is ASCII.
    Local<String> cons =
        v8_compile("cons")->Run(env.local()).ToLocalChecked().As<String>();
    CHECK_EQ(128, cons->Utf8Length(isolate));
    int nchars = -1;
    CHECK_EQ(129, cons->WriteUtf8(isolate, utf_buffer, -1, &nchars));
    CHECK_EQ(128, nchars);
    CHECK_EQ(0, strcmp(
        utf_buffer,
        "Now is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the party"));

    // Now do some stuff to make sure the strings are flattened, etc.
    CompileRun(
        "/[^a-z]/.test(cons);"
        "/[^a-z]/.test(slice);"
        "/[^a-z]/.test(slice_on_cons);");
    const char* expected_cons =
        "Now is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the party";
    const char* expected_slice =
        "ow is the time for all good men to come to the aid of the part";
    const char* expected_slice_on_cons =
        "ow is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the part";
    CHECK(v8_str(expected_cons)
              ->Equals(env.local(), env->Global()
                                        ->Get(env.local(), v8_str("cons"))
                                        .ToLocalChecked())
              .FromJust());
    CHECK(v8_str(expected_slice)
              ->Equals(env.local(), env->Global()
                                        ->Get(env.local(), v8_str("slice"))
                                        .ToLocalChecked())
              .FromJust());
    CHECK(v8_str(expected_slice_on_cons)
              ->Equals(env.local(),
                       env->Global()
                           ->Get(env.local(), v8_str("slice_on_cons"))
                           .ToLocalChecked())
              .FromJust());

    // This avoids the GC from trying to free a stack allocated resource.
    if (ilhs.IsExternalOneByteString())
      i::ExternalOneByteString::cast(ilhs).SetResource(i_isolate, nullptr);
    else
      i::ExternalTwoByteString::cast(ilhs).SetResource(i_isolate, nullptr);
    if (irhs.IsExternalOneByteString())
      i::ExternalOneByteString::cast(irhs).SetResource(i_isolate, nullptr);
    else
      i::ExternalTwoByteString::cast(irhs).SetResource(i_isolate, nullptr);
  }
  i::DeleteArray(two_byte_string);
}


TEST(CompileExternalTwoByteSource) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // This is a very short list of sources, which currently is to check for a
  // regression caused by r2703.
  const char* one_byte_sources[] = {
      "0.5",
      "-0.5",   // This mainly testes PushBack in the Scanner.
      "--0.5",  // This mainly testes PushBack in the Scanner.
      nullptr};

  // Compile the sources as external two byte strings.
  for (int i = 0; one_byte_sources[i] != nullptr; i++) {
    uint16_t* two_byte_string = AsciiToTwoByteString(one_byte_sources[i]);
    TestResource* uc16_resource = new TestResource(two_byte_string);
    v8::Local<v8::String> source =
        v8::String::NewExternalTwoByte(context->GetIsolate(), uc16_resource)
            .ToLocalChecked();
    v8::Script::Compile(context.local(), source).FromMaybe(Local<Script>());
  }
}

// Test that we cannot set a property on the global object if there
// is a read-only property in the prototype chain.
TEST(ReadOnlyPropertyInGlobalProto) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  LocalContext context(nullptr, templ);
  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::Object> global_proto = v8::Local<v8::Object>::Cast(
      global->Get(context.local(), v8_str("__proto__")).ToLocalChecked());
  global_proto->DefineOwnProperty(context.local(), v8_str("x"),
                                  v8::Integer::New(isolate, 0), v8::ReadOnly)
      .FromJust();
  global_proto->DefineOwnProperty(context.local(), v8_str("y"),
                                  v8::Integer::New(isolate, 0), v8::ReadOnly)
      .FromJust();
  // Check without 'eval' or 'with'.
  v8::Local<v8::Value> res =
      CompileRun("function f() { x = 42; return x; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(context.local(), res).FromJust());
  // Check with 'eval'.
  res = CompileRun("function f() { eval('1'); y = 43; return y; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(context.local(), res).FromJust());
  // Check with 'with'.
  res = CompileRun("function f() { with (this) { y = 44 }; return y; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(context.local(), res).FromJust());
}


TEST(CreateDataProperty) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  CompileRun(
      "var a = {};"
      "var b = [];"
      "Object.defineProperty(a, 'foo', {value: 23});"
      "Object.defineProperty(a, 'bar', {value: 23, configurable: true});");

  v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(
      env->Global()->Get(env.local(), v8_str("a")).ToLocalChecked());
  v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(
      env->Global()->Get(env.local(), v8_str("b")).ToLocalChecked());
  {
    // Can't change a non-configurable properties.
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->CreateDataProperty(env.local(), v8_str("foo"),
                                   v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    CHECK(obj->CreateDataProperty(env.local(), v8_str("bar"),
                                  v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val =
        obj->Get(env.local(), v8_str("bar")).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set a regular property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->CreateDataProperty(env.local(), v8_str("blub"),
                                  v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val =
        obj->Get(env.local(), v8_str("blub")).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set an indexed property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->CreateDataProperty(env.local(), v8_str("1"),
                                  v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), 1).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Special cases for arrays.
    v8::TryCatch try_catch(isolate);
    CHECK(!arr->CreateDataProperty(env.local(), v8_str("length"),
                                   v8::Integer::New(isolate, 1)).FromJust());
    CHECK(!try_catch.HasCaught());
  }
  {
    // Special cases for arrays: index exceeds the array's length
    v8::TryCatch try_catch(isolate);
    CHECK(arr->CreateDataProperty(env.local(), 1, v8::Integer::New(isolate, 23))
              .FromJust());
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(2U, arr->Length());
    v8::Local<v8::Value> val = arr->Get(env.local(), 1).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(23.0, val->NumberValue(env.local()).FromJust());

    // Set an existing entry.
    CHECK(arr->CreateDataProperty(env.local(), 0, v8::Integer::New(isolate, 42))
              .FromJust());
    CHECK(!try_catch.HasCaught());
    val = arr->Get(env.local(), 0).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  CompileRun("Object.freeze(a);");
  {
    // Can't change non-extensible objects.
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->CreateDataProperty(env.local(), v8_str("baz"),
                                   v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallback(AccessAlwaysBlocked);
  v8::Local<v8::Object> access_checked =
      templ->NewInstance(env.local()).ToLocalChecked();
  {
    v8::TryCatch try_catch(isolate);
    CHECK(access_checked->CreateDataProperty(env.local(), v8_str("foo"),
                                             v8::Integer::New(isolate, 42))
              .IsNothing());
    CHECK(try_catch.HasCaught());
  }
}


TEST(DefineOwnProperty) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  CompileRun(
      "var a = {};"
      "var b = [];"
      "Object.defineProperty(a, 'foo', {value: 23});"
      "Object.defineProperty(a, 'bar', {value: 23, configurable: true});");

  v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(
      env->Global()->Get(env.local(), v8_str("a")).ToLocalChecked());
  v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(
      env->Global()->Get(env.local(), v8_str("b")).ToLocalChecked());
  {
    // Can't change a non-configurable properties.
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->DefineOwnProperty(env.local(), v8_str("foo"),
                                  v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    CHECK(obj->DefineOwnProperty(env.local(), v8_str("bar"),
                                 v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val =
        obj->Get(env.local(), v8_str("bar")).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set a regular property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineOwnProperty(env.local(), v8_str("blub"),
                                 v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val =
        obj->Get(env.local(), v8_str("blub")).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set an indexed property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineOwnProperty(env.local(), v8_str("1"),
                                 v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), 1).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Special cases for arrays.
    v8::TryCatch try_catch(isolate);
    CHECK(!arr->DefineOwnProperty(env.local(), v8_str("length"),
                                  v8::Integer::New(isolate, 1)).FromJust());
    CHECK(!try_catch.HasCaught());
  }
  {
    // Special cases for arrays: index exceeds the array's length
    v8::TryCatch try_catch(isolate);
    CHECK(arr->DefineOwnProperty(env.local(), v8_str("1"),
                                 v8::Integer::New(isolate, 23)).FromJust());
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(2U, arr->Length());
    v8::Local<v8::Value> val = arr->Get(env.local(), 1).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(23.0, val->NumberValue(env.local()).FromJust());

    // Set an existing entry.
    CHECK(arr->DefineOwnProperty(env.local(), v8_str("0"),
                                 v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
    val = arr->Get(env.local(), 0).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set a non-writable property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineOwnProperty(env.local(), v8_str("lala"),
                                 v8::Integer::New(isolate, 42),
                                 v8::ReadOnly).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val =
        obj->Get(env.local(), v8_str("lala")).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
    CHECK_EQ(v8::ReadOnly, obj->GetPropertyAttributes(
                                    env.local(), v8_str("lala")).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  CompileRun("Object.freeze(a);");
  {
    // Can't change non-extensible objects.
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->DefineOwnProperty(env.local(), v8_str("baz"),
                                  v8::Integer::New(isolate, 42)).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallback(AccessAlwaysBlocked);
  v8::Local<v8::Object> access_checked =
      templ->NewInstance(env.local()).ToLocalChecked();
  {
    v8::TryCatch try_catch(isolate);
    CHECK(access_checked->DefineOwnProperty(env.local(), v8_str("foo"),
                                            v8::Integer::New(isolate, 42))
              .IsNothing());
    CHECK(try_catch.HasCaught());
  }
}

TEST(DefineProperty) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::Name> p;

  CompileRun(
      "var a = {};"
      "var b = [];"
      "Object.defineProperty(a, 'v1', {value: 23});"
      "Object.defineProperty(a, 'v2', {value: 23, configurable: true});");

  v8::Local<v8::Object> obj = v8::Local<v8::Object>::Cast(
      env->Global()->Get(env.local(), v8_str("a")).ToLocalChecked());
  v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(
      env->Global()->Get(env.local(), v8_str("b")).ToLocalChecked());

  v8::PropertyDescriptor desc(v8_num(42));
  {
    // Use a data descriptor.

    // Cannot change a non-configurable property.
    p = v8_str("v1");
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(23.0, val->NumberValue(env.local()).FromJust());

    // Change a configurable property.
    p = v8_str("v2");
    obj->DefineProperty(env.local(), p, desc).FromJust();
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());
    val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());

    // Check that missing writable has default value false.
    p = v8_str("v12");
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());
    val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
    v8::PropertyDescriptor desc2(v8_num(43));
    CHECK(!obj->DefineProperty(env.local(), p, desc2).FromJust());
    val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Set a regular property.
    p = v8_str("v3");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Set an indexed property.
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), v8_str("1"), desc).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), 1).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // No special case when changing array length.
    v8::TryCatch try_catch(isolate);
    // Use a writable descriptor, otherwise the next test, that changes
    // the array length will fail.
    v8::PropertyDescriptor desc(v8_num(42), true);
    CHECK(arr->DefineProperty(env.local(), v8_str("length"), desc).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Special cases for arrays: index exceeds the array's length.
    v8::TryCatch try_catch(isolate);
    CHECK(arr->DefineProperty(env.local(), v8_str("100"), desc).FromJust());
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(101U, arr->Length());
    v8::Local<v8::Value> val = arr->Get(env.local(), 100).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());

    // Set an existing entry.
    CHECK(arr->DefineProperty(env.local(), v8_str("0"), desc).FromJust());
    CHECK(!try_catch.HasCaught());
    val = arr->Get(env.local(), 0).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
  }

  {
    // Use a generic descriptor.
    v8::PropertyDescriptor desc_generic;

    p = v8_str("v4");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc_generic).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsUndefined());

    obj->Set(env.local(), p, v8_num(1)).FromJust();
    CHECK(!try_catch.HasCaught());

    val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsUndefined());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Use a data descriptor with undefined value.
    v8::PropertyDescriptor desc_empty(v8::Undefined(isolate));

    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc_empty).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsUndefined());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Use a descriptor with attribute == v8::ReadOnly.
    v8::PropertyDescriptor desc_read_only(v8_num(42), false);
    desc_read_only.set_enumerable(true);
    desc_read_only.set_configurable(true);

    p = v8_str("v5");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc_read_only).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(42.0, val->NumberValue(env.local()).FromJust());
    CHECK_EQ(v8::ReadOnly,
             obj->GetPropertyAttributes(env.local(), p).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Use an accessor descriptor with empty handles.
    v8::PropertyDescriptor desc_empty(v8::Undefined(isolate),
                                      v8::Undefined(isolate));

    p = v8_str("v6");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc_empty).FromJust());
    CHECK(!try_catch.HasCaught());
    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsUndefined());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Use an accessor descriptor.
    CompileRun(
        "var set = function(x) {this.val = 2*x;};"
        "var get = function() {return this.val || 0;};");

    v8::Local<v8::Function> get = v8::Local<v8::Function>::Cast(
        env->Global()->Get(env.local(), v8_str("get")).ToLocalChecked());
    v8::Local<v8::Function> set = v8::Local<v8::Function>::Cast(
        env->Global()->Get(env.local(), v8_str("set")).ToLocalChecked());
    v8::PropertyDescriptor desc(get, set);

    p = v8_str("v7");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());

    v8::Local<v8::Value> val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(0.0, val->NumberValue(env.local()).FromJust());
    CHECK(!try_catch.HasCaught());

    obj->Set(env.local(), p, v8_num(7)).FromJust();
    CHECK(!try_catch.HasCaught());

    val = obj->Get(env.local(), p).ToLocalChecked();
    CHECK(val->IsNumber());
    CHECK_EQ(14.0, val->NumberValue(env.local()).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Redefine an existing property.

    // desc = {value: 42, enumerable: true}
    v8::PropertyDescriptor desc(v8_num(42));
    desc.set_enumerable(true);

    p = v8_str("v8");
    v8::TryCatch try_catch(isolate);
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc = {enumerable: true}
    v8::PropertyDescriptor desc_true((v8::Local<v8::Value>()));
    desc_true.set_enumerable(true);

    // Successful redefinition because all present attributes have the same
    // value as the current descriptor.
    CHECK(obj->DefineProperty(env.local(), p, desc_true).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc = {}
    v8::PropertyDescriptor desc_empty;
    // Successful redefinition because no attributes are overwritten in the
    // current descriptor.
    CHECK(obj->DefineProperty(env.local(), p, desc_empty).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc = {enumerable: false}
    v8::PropertyDescriptor desc_false((v8::Local<v8::Value>()));
    desc_false.set_enumerable(false);
    // Not successful because we cannot define a different value for enumerable.
    CHECK(!obj->DefineProperty(env.local(), p, desc_false).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  {
    // Redefine a property that has a getter.
    CompileRun("var get = function() {};");
    v8::Local<v8::Function> get = v8::Local<v8::Function>::Cast(
        env->Global()->Get(env.local(), v8_str("get")).ToLocalChecked());

    // desc = {get: function() {}}
    v8::PropertyDescriptor desc(get, v8::Local<v8::Function>());
    v8::TryCatch try_catch(isolate);

    p = v8_str("v9");
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc_empty = {}
    // Successful because we are not redefining the current getter.
    v8::PropertyDescriptor desc_empty;
    CHECK(obj->DefineProperty(env.local(), p, desc_empty).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc = {get: function() {}}
    // Successful because we redefine the getter with its current value.
    CHECK(obj->DefineProperty(env.local(), p, desc).FromJust());
    CHECK(!try_catch.HasCaught());

    // desc = {get: undefined}
    v8::PropertyDescriptor desc_undefined(v8::Undefined(isolate),
                                          v8::Local<v8::Function>());
    // Not successful because we cannot redefine with the current value of get
    // with undefined.
    CHECK(!obj->DefineProperty(env.local(), p, desc_undefined).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  CompileRun("Object.freeze(a);");
  {
    // We cannot change non-extensible objects.
    v8::TryCatch try_catch(isolate);
    CHECK(!obj->DefineProperty(env.local(), v8_str("v10"), desc).FromJust());
    CHECK(!try_catch.HasCaught());
  }

  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallback(AccessAlwaysBlocked);
  v8::Local<v8::Object> access_checked =
      templ->NewInstance(env.local()).ToLocalChecked();
  {
    v8::TryCatch try_catch(isolate);
    CHECK(access_checked->DefineProperty(env.local(), v8_str("v11"), desc)
              .IsNothing());
    CHECK(try_catch.HasCaught());
  }
}

THREADED_TEST(GetCurrentContextWhenNotInContext) {
  i::Isolate* isolate = CcTest::i_isolate();
  CHECK_NOT_NULL(isolate);
  CHECK(isolate->context().is_null());
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::HandleScope scope(v8_isolate);
  // The following should not crash, but return an empty handle.
  v8::Local<v8::Context> current = v8_isolate->GetCurrentContext();
  CHECK(current.IsEmpty());
}


// Check that a variable declaration with no explicit initialization
// value does shadow an existing property in the prototype chain.
THREADED_TEST(InitGlobalVarInProtoChain) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  // Introduce a variable in the prototype chain.
  CompileRun("__proto__.x = 42");
  v8::Local<v8::Value> result = CompileRun("var x = 43; x");
  CHECK(!result->IsUndefined());
  CHECK_EQ(43, result->Int32Value(context.local()).FromJust());
}


// Regression test for issue 398.
// If a function is added to an object, creating a constant function
// field, and the result is cloned, replacing the constant function on the
// original should not affect the clone.
// See http://code.google.com/p/v8/issues/detail?id=398
THREADED_TEST(ReplaceConstantFunction) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Local<v8::FunctionTemplate> func_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Local<v8::String> foo_string = v8_str("foo");
  obj->Set(context.local(), foo_string,
           func_templ->GetFunction(context.local()).ToLocalChecked())
      .FromJust();
  v8::Local<v8::Object> obj_clone = obj->Clone();
  obj_clone->Set(context.local(), foo_string, v8_str("Hello")).FromJust();
  CHECK(!obj->Get(context.local(), foo_string).ToLocalChecked()->IsUndefined());
}

THREADED_TEST(ScriptContextDependence) {
  LocalContext c1;
  v8::HandleScope scope(c1->GetIsolate());
  const char *source = "foo";
  v8::Local<v8::Script> dep = v8_compile(source);
  v8::ScriptCompiler::Source script_source(
      v8::String::NewFromUtf8(c1->GetIsolate(), source,
                              v8::NewStringType::kNormal)
          .ToLocalChecked());
  v8::Local<v8::UnboundScript> indep =
      v8::ScriptCompiler::CompileUnboundScript(c1->GetIsolate(), &script_source)
          .ToLocalChecked();
  c1->Global()
      ->Set(c1.local(), v8::String::NewFromUtf8(c1->GetIsolate(), "foo",
                                                v8::NewStringType::kNormal)
                            .ToLocalChecked(),
            v8::Integer::New(c1->GetIsolate(), 100))
      .FromJust();
  CHECK_EQ(
      dep->Run(c1.local()).ToLocalChecked()->Int32Value(c1.local()).FromJust(),
      100);
  CHECK_EQ(indep->BindToCurrentContext()
               ->Run(c1.local())
               .ToLocalChecked()
               ->Int32Value(c1.local())
               .FromJust(),
           100);
  LocalContext c2;
  c2->Global()
      ->Set(c2.local(), v8::String::NewFromUtf8(c2->GetIsolate(), "foo",
                                                v8::NewStringType::kNormal)
                            .ToLocalChecked(),
            v8::Integer::New(c2->GetIsolate(), 101))
      .FromJust();
  CHECK_EQ(
      dep->Run(c2.local()).ToLocalChecked()->Int32Value(c2.local()).FromJust(),
      100);
  CHECK_EQ(indep->BindToCurrentContext()
               ->Run(c2.local())
               .ToLocalChecked()
               ->Int32Value(c2.local())
               .FromJust(),
           101);
}


static int asm_warning_triggered = 0;

static void AsmJsWarningListener(v8::Local<v8::Message> message,
                                 v8::Local<Value>) {
  DCHECK_EQ(v8::Isolate::kMessageWarning, message->ErrorLevel());
  asm_warning_triggered = 1;
}

TEST(AsmJsWarning) {
  i::FLAG_validate_asm = true;
  if (i::FLAG_suppress_asm_messages) return;

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  asm_warning_triggered = 0;
  isolate->AddMessageListenerWithErrorLevel(AsmJsWarningListener,
                                            v8::Isolate::kMessageAll);
  CompileRun(
      "function module() {\n"
      "  'use asm';\n"
      "  var x = 'hi';\n"
      "  return {};\n"
      "}\n"
      "module();");
  DCHECK_EQ(1, asm_warning_triggered);
  isolate->RemoveMessageListeners(AsmJsWarningListener);
}

static int error_level_message_count = 0;
static int expected_error_level = 0;

static void ErrorLevelListener(v8::Local<v8::Message> message,
                               v8::Local<Value>) {
  DCHECK_EQ(expected_error_level, message->ErrorLevel());
  ++error_level_message_count;
}

TEST(ErrorLevelWarning) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::HandleScope scope(isolate);

  const char* source = "fake = 1;";
  v8::Local<v8::Script> lscript = CompileWithOrigin(source, "test", false);
  i::Handle<i::SharedFunctionInfo> obj = i::Handle<i::SharedFunctionInfo>::cast(
      v8::Utils::OpenHandle(*lscript->GetUnboundScript()));
  CHECK(obj->script().IsScript());
  i::Handle<i::Script> script(i::Script::cast(obj->script()), i_isolate);

  int levels[] = {
      v8::Isolate::kMessageLog, v8::Isolate::kMessageInfo,
      v8::Isolate::kMessageDebug, v8::Isolate::kMessageWarning,
  };
  error_level_message_count = 0;
  isolate->AddMessageListenerWithErrorLevel(ErrorLevelListener,
                                            v8::Isolate::kMessageAll);
  for (size_t i = 0; i < arraysize(levels); i++) {
    i::MessageLocation location(script, 0, 0);
    i::Handle<i::String> msg(
        i_isolate->factory()->InternalizeString(i::StaticCharVector("test")));
    i::Handle<i::JSMessageObject> message =
        i::MessageHandler::MakeMessageObject(
            i_isolate, i::MessageTemplate::kAsmJsInvalid, &location, msg,
            i::Handle<i::FixedArray>::null());
    message->set_error_level(levels[i]);
    expected_error_level = levels[i];
    i::MessageHandler::ReportMessage(i_isolate, &location, message);
  }
  isolate->RemoveMessageListeners(ErrorLevelListener);
  DCHECK_EQ(arraysize(levels), error_level_message_count);
}

v8::PromiseRejectEvent reject_event = v8::kPromiseRejectWithNoHandler;
int promise_reject_counter = 0;
int promise_revoke_counter = 0;
int promise_reject_after_resolved_counter = 0;
int promise_resolve_after_resolved_counter = 0;
int promise_reject_msg_line_number = -1;
int promise_reject_msg_column_number = -1;
int promise_reject_line_number = -1;
int promise_reject_column_number = -1;
int promise_reject_frame_count = -1;
bool promise_reject_is_shared_cross_origin = false;

void PromiseRejectCallback(v8::PromiseRejectMessage reject_message) {
  v8::Local<v8::Object> global = CcTest::global();
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  CHECK_NE(v8::Promise::PromiseState::kPending,
           reject_message.GetPromise()->State());
  switch (reject_message.GetEvent()) {
    case v8::kPromiseRejectWithNoHandler: {
      promise_reject_counter++;
      global->Set(context, v8_str("rejected"), reject_message.GetPromise())
          .FromJust();
      global->Set(context, v8_str("value"), reject_message.GetValue())
          .FromJust();
      v8::Local<v8::Message> message = v8::Exception::CreateMessage(
          CcTest::isolate(), reject_message.GetValue());
      v8::Local<v8::StackTrace> stack_trace = message->GetStackTrace();

      promise_reject_msg_line_number =
          message->GetLineNumber(context).FromJust();
      promise_reject_msg_column_number =
          message->GetStartColumn(context).FromJust() + 1;
      promise_reject_is_shared_cross_origin =
          message->IsSharedCrossOrigin();

      if (!stack_trace.IsEmpty()) {
        promise_reject_frame_count = stack_trace->GetFrameCount();
        if (promise_reject_frame_count > 0) {
          CHECK(stack_trace->GetFrame(CcTest::isolate(), 0)
                    ->GetScriptName()
                    ->Equals(context, v8_str("pro"))
                    .FromJust());
          promise_reject_line_number =
              stack_trace->GetFrame(CcTest::isolate(), 0)->GetLineNumber();
          promise_reject_column_number =
              stack_trace->GetFrame(CcTest::isolate(), 0)->GetColumn();
        } else {
          promise_reject_line_number = -1;
          promise_reject_column_number = -1;
        }
      }
      break;
    }
    case v8::kPromiseHandlerAddedAfterReject: {
      promise_revoke_counter++;
      global->Set(context, v8_str("revoked"), reject_message.GetPromise())
          .FromJust();
      CHECK(reject_message.GetValue().IsEmpty());
      break;
    }
    case v8::kPromiseRejectAfterResolved: {
      promise_reject_after_resolved_counter++;
      break;
    }
    case v8::kPromiseResolveAfterResolved: {
      promise_resolve_after_resolved_counter++;
      break;
    }
  }
}


v8::Local<v8::Promise> GetPromise(const char* name) {
  return v8::Local<v8::Promise>::Cast(
      CcTest::global()
          ->Get(CcTest::isolate()->GetCurrentContext(), v8_str(name))
          .ToLocalChecked());
}


v8::Local<v8::Value> RejectValue() {
  return CcTest::global()
      ->Get(CcTest::isolate()->GetCurrentContext(), v8_str("value"))
      .ToLocalChecked();
}


void ResetPromiseStates() {
  promise_reject_counter = 0;
  promise_revoke_counter = 0;
  promise_reject_after_resolved_counter = 0;
  promise_resolve_after_resolved_counter = 0;
  promise_reject_msg_line_number = -1;
  promise_reject_msg_column_number = -1;
  promise_reject_line_number = -1;
  promise_reject_column_number = -1;
  promise_reject_frame_count = -1;

  v8::Local<v8::Object> global = CcTest::global();
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  global->Set(context, v8_str("rejected"), v8_str("")).FromJust();
  global->Set(context, v8_str("value"), v8_str("")).FromJust();
  global->Set(context, v8_str("revoked"), v8_str("")).FromJust();
}


TEST(PromiseRejectCallback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetPromiseRejectCallback(PromiseRejectCallback);

  ResetPromiseStates();

  // Create promise p0.
  CompileRun(
      "var reject;            \n"
      "var p0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("p0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add resolve handler (and default reject handler) to p0.
  CompileRun("var p1 = p0.then(function(){});");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject p0.
  CompileRun("reject('ppp');");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(v8::kPromiseRejectWithNoHandler, reject_event);
  CHECK(
      GetPromise("rejected")->Equals(env.local(), GetPromise("p1")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("ppp")).FromJust());

  // Reject p0 again. Callback is not triggered again.
  CompileRun("reject();");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add resolve handler to p1.
  CompileRun("var p2 = p1.then(function(){});");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(GetPromise("p1")->HasHandler());
  CHECK(!GetPromise("p2")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(
      GetPromise("rejected")->Equals(env.local(), GetPromise("p2")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("ppp")).FromJust());
  CHECK(
      GetPromise("revoked")->Equals(env.local(), GetPromise("p1")).FromJust());

  ResetPromiseStates();

  // Create promise q0.
  CompileRun(
      "var q0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("q0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add reject handler to q0.
  CompileRun("var q1 = q0.catch(function() {});");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject q0.
  CompileRun("reject('qq')");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add a new reject handler, which rejects by returning Promise.reject().
  // The returned promise q_ triggers a reject callback at first, only to
  // revoke it when returning it causes q2 to be rejected.
  CompileRun(
      "var q_;"
      "var q2 = q0.catch(               \n"
      "   function() {                  \n"
      "     q_ = Promise.reject('qqq'); \n"
      "     return q_;                  \n"
      "   }                             \n"
      ");                               \n");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK(!GetPromise("q2")->HasHandler());
  CHECK(GetPromise("q_")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(
      GetPromise("rejected")->Equals(env.local(), GetPromise("q2")).FromJust());
  CHECK(
      GetPromise("revoked")->Equals(env.local(), GetPromise("q_")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("qqq")).FromJust());

  // Add a reject handler to the resolved q1, which rejects by throwing.
  CompileRun(
      "var q3 = q1.then(  \n"
      "   function() {    \n"
      "     throw 'qqqq'; \n"
      "   }               \n"
      ");                 \n");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(GetPromise("q1")->HasHandler());
  CHECK(!GetPromise("q2")->HasHandler());
  CHECK(!GetPromise("q3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(
      GetPromise("rejected")->Equals(env.local(), GetPromise("q3")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("qqqq")).FromJust());

  ResetPromiseStates();

  // Create promise r0, which has three handlers, two of which handle rejects.
  CompileRun(
      "var r0 = new Promise(             \n"
      "  function(res, rej) {            \n"
      "    reject = rej;                 \n"
      "  }                               \n"
      ");                                \n"
      "var r1 = r0.catch(function() {}); \n"
      "var r2 = r0.then(function() {});  \n"
      "var r3 = r0.then(function() {},   \n"
      "                 function() {});  \n");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(!GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject r0.
  CompileRun("reject('rrr')");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(!GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK(
      GetPromise("rejected")->Equals(env.local(), GetPromise("r2")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("rrr")).FromJust());

  // Add reject handler to r2.
  CompileRun("var r4 = r2.catch(function() {});");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK(!GetPromise("r4")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(
      GetPromise("revoked")->Equals(env.local(), GetPromise("r2")).FromJust());
  CHECK(RejectValue()->Equals(env.local(), v8_str("rrr")).FromJust());

  // Add reject handlers to r4.
  CompileRun("var r5 = r4.then(function() {}, function() {});");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK(GetPromise("r4")->HasHandler());
  CHECK(!GetPromise("r5")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);

  ResetPromiseStates();

  // Create promise s0, which has three handlers, none of which handle rejects.
  CompileRun(
      "var s0 = new Promise(            \n"
      "  function(res, rej) {           \n"
      "    reject = rej;                \n"
      "  }                              \n"
      ");                               \n"
      "var s1 = s0.then(function() {}); \n"
      "var s2 = s0.then(function() {}); \n"
      "var s3 = s0.then(function() {}); \n");
  CHECK(GetPromise("s0")->HasHandler());
  CHECK(!GetPromise("s1")->HasHandler());
  CHECK(!GetPromise("s2")->HasHandler());
  CHECK(!GetPromise("s3")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject s0.
  CompileRun("reject('sss')");
  CHECK(GetPromise("s0")->HasHandler());
  CHECK(!GetPromise("s1")->HasHandler());
  CHECK(!GetPromise("s2")->HasHandler());
  CHECK(!GetPromise("s3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK(RejectValue()->Equals(env.local(), v8_str("sss")).FromJust());

  ResetPromiseStates();

  // Swallowed exceptions in the Promise constructor.
  CompileRun(
      "var v0 = new Promise(\n"
      "  function(res, rej) {\n"
      "    res(1);\n"
      "    throw new Error();\n"
      "  }\n"
      ");\n");
  CHECK(!GetPromise("v0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(1, promise_reject_after_resolved_counter);
  CHECK_EQ(0, promise_resolve_after_resolved_counter);

  ResetPromiseStates();

  // Duplication resolve.
  CompileRun(
      "var r;\n"
      "var y0 = new Promise(\n"
      "  function(res, rej) {\n"
      "    r = res;\n"
      "    throw new Error();\n"
      "  }\n"
      ");\n"
      "r(1);\n");
  CHECK(!GetPromise("y0")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(0, promise_reject_after_resolved_counter);
  CHECK_EQ(1, promise_resolve_after_resolved_counter);

  // Test stack frames.
  env->GetIsolate()->SetCaptureStackTraceForUncaughtExceptions(true);

  ResetPromiseStates();

  // Create promise t0, which is rejected in the constructor with an error.
  CompileRunWithOrigin(
      "var t0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reference_error;   \n"
      "  }                    \n"
      ");                     \n",
      "pro", 0, 0);
  CHECK(!GetPromise("t0")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(3, promise_reject_line_number);
  CHECK_EQ(5, promise_reject_column_number);
  CHECK_EQ(3, promise_reject_msg_line_number);
  CHECK_EQ(5, promise_reject_msg_column_number);

  ResetPromiseStates();

  // Create promise u0 and chain u1 to it, which is rejected via throw.
  CompileRunWithOrigin(
      "var u0 = Promise.resolve();        \n"
      "var u1 = u0.then(                  \n"
      "           function() {            \n"
      "             (function() {         \n"
      "                throw new Error(); \n"
      "              })();                \n"
      "           }                       \n"
      "         );                        \n",
      "pro", 0, 0);
  CHECK(GetPromise("u0")->HasHandler());
  CHECK(!GetPromise("u1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(5, promise_reject_line_number);
  CHECK_EQ(23, promise_reject_column_number);
  CHECK_EQ(5, promise_reject_msg_line_number);
  CHECK_EQ(23, promise_reject_msg_column_number);

  // Throw in u3, which handles u1's rejection.
  CompileRunWithOrigin(
      "function f() {                \n"
      "  return (function() {        \n"
      "    return new Error();       \n"
      "  })();                       \n"
      "}                             \n"
      "var u2 = Promise.reject(f()); \n"
      "var u3 = u1.catch(            \n"
      "           function() {       \n"
      "             return u2;       \n"
      "           }                  \n"
      "         );                   \n",
      "pro", 0, 0);
  CHECK(GetPromise("u0")->HasHandler());
  CHECK(GetPromise("u1")->HasHandler());
  CHECK(GetPromise("u2")->HasHandler());
  CHECK(!GetPromise("u3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(2, promise_revoke_counter);
  CHECK_EQ(3, promise_reject_frame_count);
  CHECK_EQ(3, promise_reject_line_number);
  CHECK_EQ(12, promise_reject_column_number);
  CHECK_EQ(3, promise_reject_msg_line_number);
  CHECK_EQ(12, promise_reject_msg_column_number);

  ResetPromiseStates();

  // Create promise rejected promise v0, which is incorrectly handled by v1
  // via chaining cycle.
  CompileRunWithOrigin(
      "var v0 = Promise.reject(); \n"
      "var v1 = v0.catch(         \n"
      "           function() {    \n"
      "             return v1;    \n"
      "           }               \n"
      "         );                \n",
      "pro", 0, 0);
  CHECK(GetPromise("v0")->HasHandler());
  CHECK(!GetPromise("v1")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK_EQ(0, promise_reject_frame_count);
  CHECK_EQ(-1, promise_reject_line_number);
  CHECK_EQ(-1, promise_reject_column_number);

  ResetPromiseStates();

  // Create promise t1, which rejects by throwing syntax error from eval.
  CompileRunWithOrigin(
      "var t1 = new Promise(   \n"
      "  function(res, rej) {  \n"
      "    var content = '\\n\\\n"
      "      }';               \n"
      "    eval(content);      \n"
      "  }                     \n"
      ");                      \n",
      "pro", 0, 0);
  CHECK(!GetPromise("t1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(5, promise_reject_line_number);
  CHECK_EQ(10, promise_reject_column_number);
  CHECK_EQ(2, promise_reject_msg_line_number);
  CHECK_EQ(7, promise_reject_msg_column_number);
}

TEST(PromiseRejectIsSharedCrossOrigin) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetPromiseRejectCallback(PromiseRejectCallback);

  ResetPromiseStates();

  // Create promise p0.
  CompileRun(
      "var reject;            \n"
      "var p0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("p0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  // Not set because it's not yet rejected.
  CHECK(!promise_reject_is_shared_cross_origin);

  // Reject p0.
  CompileRun("reject('ppp');");
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  // Not set because the ScriptOriginOptions is from the script.
  CHECK(!promise_reject_is_shared_cross_origin);

  ResetPromiseStates();

  // Create promise p1
  CompileRun(
      "var reject;            \n"
      "var p1 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  // Not set because it's not yet rejected.
  CHECK(!promise_reject_is_shared_cross_origin);

  // Add resolve handler (and default reject handler) to p1.
  CompileRun("var p2 = p1.then(function(){});");
  CHECK(GetPromise("p1")->HasHandler());
  CHECK(!GetPromise("p2")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject p1.
  CompileRun("reject('ppp');");
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  // Set because the event is from an empty script.
  CHECK(promise_reject_is_shared_cross_origin);
}

TEST(PromiseRejectMarkAsHandled) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetPromiseRejectCallback(PromiseRejectCallback);

  ResetPromiseStates();

  // Create promise p0.
  CompileRun(
      "var reject;            \n"
      "var p0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("p0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  GetPromise("p0")->MarkAsHandled();

  // Reject p0. promise_reject_counter shouldn't be incremented because
  // it's marked as handled.
  CompileRun("reject('ppp');");
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
}
void PromiseRejectCallbackConstructError(
    v8::PromiseRejectMessage reject_message) {
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  CHECK_EQ(v8::Promise::PromiseState::kRejected,
           reject_message.GetPromise()->State());
  USE(v8::Script::Compile(context, v8_str("new Error('test')"))
          .ToLocalChecked()
          ->Run(context));
}

TEST(PromiseRejectCallbackConstructError) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetPromiseRejectCallback(PromiseRejectCallbackConstructError);

  ResetPromiseStates();
  CompileRun(
      "function f(p) {"
      "    p.catch(() => {});"
      "};"
      "%PrepareFunctionForOptimization(f);"
      "f(Promise.reject());"
      "f(Promise.reject());"
      "%OptimizeFunctionOnNextCall(f);"
      "let p = Promise.reject();"
      "f(p);");
}

void SetPromise(const char* name, v8::Local<v8::Promise> promise) {
  CcTest::global()
      ->Set(CcTest::isolate()->GetCurrentContext(), v8_str(name), promise)
      .FromJust();
}

class PromiseHookData {
 public:
  int before_hook_count = 0;
  int after_hook_count = 0;
  int promise_hook_count = 0;
  int parent_promise_count = 0;
  bool check_value = true;
  std::string promise_hook_value;

  void Reset() {
    before_hook_count = 0;
    after_hook_count = 0;
    promise_hook_count = 0;
    parent_promise_count = 0;
    check_value = true;
    promise_hook_value = "";
  }
};

PromiseHookData* promise_hook_data;

void CustomPromiseHook(v8::PromiseHookType type, v8::Local<v8::Promise> promise,
                       v8::Local<v8::Value> parentPromise) {
  promise_hook_data->promise_hook_count++;
  switch (type) {
    case v8::PromiseHookType::kInit:
      SetPromise("init", promise);

      if (!parentPromise->IsUndefined()) {
        promise_hook_data->parent_promise_count++;
        SetPromise("parent", v8::Local<v8::Promise>::Cast(parentPromise));
      }

      break;
    case v8::PromiseHookType::kResolve:
      SetPromise("resolve", promise);
      break;
    case v8::PromiseHookType::kBefore:
      promise_hook_data->before_hook_count++;
      CHECK(promise_hook_data->before_hook_count >
            promise_hook_data->after_hook_count);
      CHECK(CcTest::global()
                ->Get(CcTest::isolate()->GetCurrentContext(), v8_str("value"))
                .ToLocalChecked()
                ->Equals(CcTest::isolate()->GetCurrentContext(), v8_str(""))
                .FromJust());
      SetPromise("before", promise);
      break;
    case v8::PromiseHookType::kAfter:
      promise_hook_data->after_hook_count++;
      CHECK(promise_hook_data->after_hook_count <=
            promise_hook_data->before_hook_count);
      if (promise_hook_data->check_value) {
        CHECK(
            CcTest::global()
                ->Get(CcTest::isolate()->GetCurrentContext(), v8_str("value"))
                .ToLocalChecked()
                ->Equals(CcTest::isolate()->GetCurrentContext(),
                         v8_str(promise_hook_data->promise_hook_value.c_str()))
                .FromJust());
      }
      SetPromise("after", promise);
      break;
  }
}

TEST(PromiseHook) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> global = CcTest::global();
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();

  promise_hook_data = new PromiseHookData();
  isolate->SetPromiseHook(CustomPromiseHook);

  // Test that an initialized promise is passed to init. Other hooks
  // can not have un initialized promise.
  promise_hook_data->check_value = false;
  CompileRun("var p = new Promise(() => {});");

  auto init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  auto init_promise_obj = v8::Local<v8::Promise>::Cast(init_promise);
  CHECK_EQ(init_promise_obj->State(), v8::Promise::PromiseState::kPending);
  CHECK(!init_promise_obj->HasHandler());

  promise_hook_data->Reset();
  promise_hook_data->promise_hook_value = "fulfilled";
  const char* source =
      "var resolve, value = ''; \n"
      "var p = new Promise(r => resolve = r); \n";

  CompileRun(source);
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  CHECK_EQ(1, promise_hook_data->promise_hook_count);
  CHECK_EQ(0, promise_hook_data->parent_promise_count);

  CompileRun("var p1 = p.then(() => { value = 'fulfilled'; }); \n");
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  auto parent_promise = global->Get(context, v8_str("parent")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), init_promise).FromJust());
  CHECK(GetPromise("p")->Equals(env.local(), parent_promise).FromJust());
  CHECK_EQ(2, promise_hook_data->promise_hook_count);
  CHECK_EQ(1, promise_hook_data->parent_promise_count);

  CompileRun("resolve(); \n");
  auto resolve_promise =
      global->Get(context, v8_str("resolve")).ToLocalChecked();
  auto before_promise = global->Get(context, v8_str("before")).ToLocalChecked();
  auto after_promise = global->Get(context, v8_str("after")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), before_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), after_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  CHECK_EQ(6, promise_hook_data->promise_hook_count);

  CompileRun("value = ''; var p2 = p1.then(() => { value = 'fulfilled' }); \n");
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  parent_promise = global->Get(context, v8_str("parent")).ToLocalChecked();
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  before_promise = global->Get(context, v8_str("before")).ToLocalChecked();
  after_promise = global->Get(context, v8_str("after")).ToLocalChecked();
  CHECK(GetPromise("p2")->Equals(env.local(), init_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), parent_promise).FromJust());
  CHECK(GetPromise("p2")->Equals(env.local(), before_promise).FromJust());
  CHECK(GetPromise("p2")->Equals(env.local(), after_promise).FromJust());
  CHECK(GetPromise("p2")->Equals(env.local(), resolve_promise).FromJust());
  CHECK_EQ(10, promise_hook_data->promise_hook_count);

  promise_hook_data->Reset();
  promise_hook_data->promise_hook_value = "rejected";
  source =
      "var reject, value = ''; \n"
      "var p = new Promise((_, r) => reject = r); \n";

  CompileRun(source);
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  CHECK_EQ(1, promise_hook_data->promise_hook_count);
  CHECK_EQ(0, promise_hook_data->parent_promise_count);

  CompileRun("var p1 = p.catch(() => { value = 'rejected'; }); \n");
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  parent_promise = global->Get(context, v8_str("parent")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), init_promise).FromJust());
  CHECK(GetPromise("p")->Equals(env.local(), parent_promise).FromJust());
  CHECK_EQ(2, promise_hook_data->promise_hook_count);
  CHECK_EQ(1, promise_hook_data->parent_promise_count);

  CompileRun("reject(); \n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  before_promise = global->Get(context, v8_str("before")).ToLocalChecked();
  after_promise = global->Get(context, v8_str("after")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), before_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), after_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  CHECK_EQ(6, promise_hook_data->promise_hook_count);

  promise_hook_data->Reset();
  promise_hook_data->promise_hook_value = "Promise.resolve";
  source =
      "var value = ''; \n"
      "var p = Promise.resolve('Promise.resolve'); \n";

  CompileRun(source);
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  // init hook and resolve hook
  CHECK_EQ(2, promise_hook_data->promise_hook_count);
  CHECK_EQ(0, promise_hook_data->parent_promise_count);
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), resolve_promise).FromJust());

  CompileRun("var p1 = p.then((v) => { value = v; }); \n");
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  parent_promise = global->Get(context, v8_str("parent")).ToLocalChecked();
  before_promise = global->Get(context, v8_str("before")).ToLocalChecked();
  after_promise = global->Get(context, v8_str("after")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), init_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  CHECK(GetPromise("p")->Equals(env.local(), parent_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), before_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), after_promise).FromJust());
  CHECK_EQ(6, promise_hook_data->promise_hook_count);
  CHECK_EQ(1, promise_hook_data->parent_promise_count);

  promise_hook_data->Reset();
  source =
      "var resolve, value = ''; \n"
      "var p = new Promise((_, r) => resolve = r); \n";

  CompileRun(source);
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  CHECK_EQ(1, promise_hook_data->promise_hook_count);
  CHECK_EQ(0, promise_hook_data->parent_promise_count);

  CompileRun("resolve(); \n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), resolve_promise).FromJust());
  CHECK_EQ(2, promise_hook_data->promise_hook_count);

  promise_hook_data->Reset();
  source =
      "var reject, value = ''; \n"
      "var p = new Promise((_, r) => reject = r); \n";

  CompileRun(source);
  init_promise = global->Get(context, v8_str("init")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), init_promise).FromJust());
  CHECK_EQ(1, promise_hook_data->promise_hook_count);
  CHECK_EQ(0, promise_hook_data->parent_promise_count);

  CompileRun("reject(); \n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  CHECK(GetPromise("p")->Equals(env.local(), resolve_promise).FromJust());
  CHECK_EQ(2, promise_hook_data->promise_hook_count);

  promise_hook_data->Reset();
  // This test triggers after callbacks right after each other, so
  // lets just check the value at the end.
  promise_hook_data->check_value = false;
  promise_hook_data->promise_hook_value = "Promise.all";
  source =
      "var resolve, value = ''; \n"
      "var tempPromise = new Promise(r => resolve = r); \n"
      "var p = Promise.all([tempPromise]);\n "
      "var p1 = p.then(v => value = v[0]); \n";

  CompileRun(source);
  // 1) init hook (tempPromise)
  // 2) init hook (p)
  // 3) init hook (throwaway Promise in Promise.all, p)
  // 4) init hook (p1, p)
  CHECK_EQ(4, promise_hook_data->promise_hook_count);
  CHECK_EQ(2, promise_hook_data->parent_promise_count);

  promise_hook_data->promise_hook_value = "Promise.all";
  CompileRun("resolve('Promise.all'); \n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  // 5) resolve hook (tempPromise)
  // 6) resolve hook (throwaway Promise in Promise.all)
  // 6) before hook (throwaway Promise in Promise.all)
  // 7) after hook (throwaway Promise in Promise.all)
  // 8) before hook (p)
  // 9) after hook (p)
  // 10) resolve hook (p1)
  // 11) before hook (p1)
  // 12) after hook (p1)
  CHECK_EQ(12, promise_hook_data->promise_hook_count);
  CHECK(CcTest::global()
            ->Get(CcTest::isolate()->GetCurrentContext(), v8_str("value"))
            .ToLocalChecked()
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8_str(promise_hook_data->promise_hook_value.c_str()))
            .FromJust());

  promise_hook_data->Reset();
  // This test triggers after callbacks right after each other, so
  // lets just check the value at the end.
  promise_hook_data->check_value = false;
  promise_hook_data->promise_hook_value = "Promise.race";
  source =
      "var resolve, value = ''; \n"
      "var tempPromise = new Promise(r => resolve = r); \n"
      "var p = Promise.race([tempPromise]);\n "
      "var p1 = p.then(v => value = v); \n";

  CompileRun(source);
  // 1) init hook (tempPromise)
  // 2) init hook (p)
  // 3) init hook (throwaway Promise in Promise.race, p)
  // 4) init hook (p1, p)
  CHECK_EQ(4, promise_hook_data->promise_hook_count);
  CHECK_EQ(2, promise_hook_data->parent_promise_count);

  promise_hook_data->promise_hook_value = "Promise.race";
  CompileRun("resolve('Promise.race'); \n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  // 5) resolve hook (tempPromise)
  // 6) resolve hook (throwaway Promise in Promise.race)
  // 6) before hook (throwaway Promise in Promise.race)
  // 7) after hook (throwaway Promise in Promise.race)
  // 8) before hook (p)
  // 9) after hook (p)
  // 10) resolve hook (p1)
  // 11) before hook (p1)
  // 12) after hook (p1)
  CHECK_EQ(12, promise_hook_data->promise_hook_count);
  CHECK(CcTest::global()
            ->Get(CcTest::isolate()->GetCurrentContext(), v8_str("value"))
            .ToLocalChecked()
            ->Equals(CcTest::isolate()->GetCurrentContext(),
                     v8_str(promise_hook_data->promise_hook_value.c_str()))
            .FromJust());

  promise_hook_data->Reset();
  promise_hook_data->promise_hook_value = "subclass";
  source =
      "var resolve, value = '';\n"
      "class MyPromise extends Promise { \n"
      "  then(onFulfilled, onRejected) { \n"
      "      return super.then(onFulfilled, onRejected); \n"
      "  };\n"
      "};\n"
      "var p = new MyPromise(r => resolve = r);\n";

  CompileRun(source);
  // 1) init hook (p)
  CHECK_EQ(1, promise_hook_data->promise_hook_count);

  CompileRun("var p1 = p.then(() => value = 'subclass');\n");
  // 2) init hook (p1)
  CHECK_EQ(2, promise_hook_data->promise_hook_count);

  CompileRun("resolve();\n");
  resolve_promise = global->Get(context, v8_str("resolve")).ToLocalChecked();
  before_promise = global->Get(context, v8_str("before")).ToLocalChecked();
  after_promise = global->Get(context, v8_str("after")).ToLocalChecked();
  CHECK(GetPromise("p1")->Equals(env.local(), before_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), after_promise).FromJust());
  CHECK(GetPromise("p1")->Equals(env.local(), resolve_promise).FromJust());
  // 3) resolve hook (p)
  // 4) before hook (p)
  // 5) after hook (p)
  // 6) resolve hook (p1)
  CHECK_EQ(6, promise_hook_data->promise_hook_count);

  promise_hook_data->Reset();
  source =
      "class X extends Promise {\n"
      "  static get [Symbol.species]() {\n"
      "    return Y;\n"
      "  }\n"
      "}\n"
      "class Y {\n"
      "  constructor(executor) {\n"
      "    return new Proxy(new Promise(executor), {});\n"
      "  }\n"
      "}\n"
      "var x = X.resolve().then(() => {});\n";

  CompileRun(source);

  promise_hook_data->Reset();
  source =
      "var resolve, value = '';\n"
      "var p = new Promise(r => resolve = r);\n";

  CompileRun(source);
  CHECK_EQ(v8::Promise::kPending, GetPromise("p")->State());
  CompileRun("resolve(Promise.resolve(value));\n");
  CHECK_EQ(v8::Promise::kFulfilled, GetPromise("p")->State());
  CHECK_EQ(9, promise_hook_data->promise_hook_count);

  delete promise_hook_data;
  isolate->SetPromiseHook(nullptr);
}


TEST(EvalWithSourceURLInMessageScriptResourceNameOrSourceURL) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char *source =
    "function outer() {\n"
    "  var scriptContents = \"function foo() { FAIL.FAIL; }\\\n"
    "  //# sourceURL=source_url\";\n"
    "  eval(scriptContents);\n"
    "  foo(); }\n"
    "outer();\n"
    "//# sourceURL=outer_url";

  v8::TryCatch try_catch(context->GetIsolate());
  CompileRun(source);
  CHECK(try_catch.HasCaught());

  Local<v8::Message> message = try_catch.Message();
  Local<Value> sourceURL = message->GetScriptOrigin().ResourceName();
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(context->GetIsolate(), sourceURL),
                     "source_url"));
}


TEST(RecursionWithSourceURLInMessageScriptResourceNameOrSourceURL) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char *source =
    "function outer() {\n"
    "  var scriptContents = \"function boo(){ boo(); }\\\n"
    "  //# sourceURL=source_url\";\n"
    "  eval(scriptContents);\n"
    "  boo(); }\n"
    "outer();\n"
    "//# sourceURL=outer_url";

  v8::TryCatch try_catch(context->GetIsolate());
  CompileRun(source);
  CHECK(try_catch.HasCaught());

  Local<v8::Message> message = try_catch.Message();
  Local<Value> sourceURL = message->GetScriptOrigin().ResourceName();
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(context->GetIsolate(), sourceURL),
                     "source_url"));
}


static void CreateGarbageInOldSpace() {
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::HandleScope scope(CcTest::isolate());
  i::AlwaysAllocateScope always_allocate(CcTest::i_isolate());
  for (int i = 0; i < 1000; i++) {
    factory->NewFixedArray(1000, i::AllocationType::kOld);
  }
}


// Test that idle notification can be handled and eventually collects garbage.
TEST(TestIdleNotification) {
  if (!i::FLAG_incremental_marking) return;
  ManualGCScope manual_gc_scope;
  const intptr_t MB = 1024 * 1024;
  const double IdlePauseInSeconds = 1.0;
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  intptr_t initial_size = CcTest::heap()->SizeOfObjects();
  CreateGarbageInOldSpace();
  intptr_t size_with_garbage = CcTest::heap()->SizeOfObjects();
  CHECK_GT(size_with_garbage, initial_size + MB);
  bool finished = false;
  for (int i = 0; i < 200 && !finished; i++) {
    if (i < 10 && CcTest::heap()->incremental_marking()->IsStopped()) {
      CcTest::heap()->StartIdleIncrementalMarking(
          i::GarbageCollectionReason::kTesting);
    }
    finished = env->GetIsolate()->IdleNotificationDeadline(
        (v8::base::TimeTicks::HighResolutionNow().ToInternalValue() /
         static_cast<double>(v8::base::Time::kMicrosecondsPerSecond)) +
        IdlePauseInSeconds);
    if (CcTest::heap()->mark_compact_collector()->sweeping_in_progress()) {
      CcTest::heap()->mark_compact_collector()->EnsureSweepingCompleted();
    }
  }
  intptr_t final_size = CcTest::heap()->SizeOfObjects();
  CHECK(finished);
  CHECK_LT(final_size, initial_size + 1);
}

TEST(TestMemorySavingsMode) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::internal::Isolate* i_isolate =
      reinterpret_cast<v8::internal::Isolate*>(isolate);
  CHECK(!i_isolate->IsMemorySavingsModeActive());
  isolate->EnableMemorySavingsMode();
  CHECK(i_isolate->IsMemorySavingsModeActive());
  isolate->DisableMemorySavingsMode();
  CHECK(!i_isolate->IsMemorySavingsModeActive());
}

TEST(Regress2333) {
  LocalContext env;
  for (int i = 0; i < 3; i++) {
    CcTest::CollectGarbage(i::NEW_SPACE);
  }
}

static uint32_t* stack_limit;

static void GetStackLimitCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  stack_limit = reinterpret_cast<uint32_t*>(
      CcTest::i_isolate()->stack_guard()->real_climit());
}


// Uses the address of a local variable to determine the stack top now.
// Given a size, returns an address that is that far from the current
// top of stack.
static uint32_t* ComputeStackLimit(uint32_t size) {
  uint32_t* answer = &size - (size / sizeof(size));
  // If the size is very large and the stack is very near the bottom of
  // memory then the calculation above may wrap around and give an address
  // that is above the (downwards-growing) stack.  In that case we return
  // a very low address.
  if (answer > &size) return reinterpret_cast<uint32_t*>(sizeof(size));
  return answer;
}


// We need at least 165kB for an x64 debug build with clang and ASAN.
static const int stack_breathing_room = 256 * i::KB;


TEST(SetStackLimit) {
  uint32_t* set_limit = ComputeStackLimit(stack_breathing_room);

  // Set stack limit.
  CcTest::isolate()->SetStackLimit(reinterpret_cast<uintptr_t>(set_limit));

  // Execute a script.
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), GetStackLimitCallback);
  Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
  CHECK(env->Global()
            ->Set(env.local(), v8_str("get_stack_limit"), fun)
            .FromJust());
  CompileRun("get_stack_limit();");

  CHECK(stack_limit == set_limit);
}


TEST(SetStackLimitInThread) {
  uint32_t* set_limit;
  {
    v8::Locker locker(CcTest::isolate());
    set_limit = ComputeStackLimit(stack_breathing_room);

    // Set stack limit.
    CcTest::isolate()->SetStackLimit(reinterpret_cast<uintptr_t>(set_limit));

    // Execute a script.
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), GetStackLimitCallback);
    Local<Function> fun = fun_templ->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("get_stack_limit"), fun)
              .FromJust());
    CompileRun("get_stack_limit();");

    CHECK(stack_limit == set_limit);
  }
  {
    v8::Locker locker(CcTest::isolate());
    CHECK(stack_limit == set_limit);
  }
}

THREADED_TEST(GetHeapStatistics) {
  LocalContext c1;
  v8::HandleScope scope(c1->GetIsolate());
  v8::HeapStatistics heap_statistics;
  CHECK_EQ(0u, heap_statistics.total_heap_size());
  CHECK_EQ(0u, heap_statistics.used_heap_size());
  c1->GetIsolate()->GetHeapStatistics(&heap_statistics);
  CHECK_NE(static_cast<int>(heap_statistics.total_heap_size()), 0);
  CHECK_NE(static_cast<int>(heap_statistics.used_heap_size()), 0);
}

TEST(GetHeapSpaceStatistics) {
  LocalContext c1;
  v8::Isolate* isolate = c1->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::HeapStatistics heap_statistics;

  // Force allocation in LO_SPACE so that every space has non-zero size.
  v8::internal::Isolate* i_isolate =
      reinterpret_cast<v8::internal::Isolate*>(isolate);
  auto unused = i_isolate->factory()->TryNewFixedArray(512 * 1024,
                                                       i::AllocationType::kOld);
  USE(unused);

  isolate->GetHeapStatistics(&heap_statistics);

  // Ensure that the sum of all the spaces matches the totals from
  // GetHeapSpaceStatics.
  size_t total_size = 0u;
  size_t total_used_size = 0u;
  size_t total_available_size = 0u;
  size_t total_physical_size = 0u;
  for (size_t i = 0; i < isolate->NumberOfHeapSpaces(); ++i) {
    v8::HeapSpaceStatistics space_statistics;
    isolate->GetHeapSpaceStatistics(&space_statistics, i);
    CHECK_NOT_NULL(space_statistics.space_name());
    total_size += space_statistics.space_size();
    total_used_size += space_statistics.space_used_size();
    total_available_size += space_statistics.space_available_size();
    total_physical_size += space_statistics.physical_space_size();
  }
  total_available_size += CcTest::heap()->memory_allocator()->Available();

  CHECK_EQ(total_size, heap_statistics.total_heap_size());
  CHECK_EQ(total_used_size, heap_statistics.used_heap_size());
  CHECK_EQ(total_available_size, heap_statistics.total_available_size());
  CHECK_EQ(total_physical_size, heap_statistics.total_physical_size());
}

TEST(NumberOfNativeContexts) {
  static const size_t kNumTestContexts = 10;
  i::Isolate* isolate = CcTest::i_isolate();
  i::HandleScope scope(isolate);
  v8::Global<v8::Context> context[kNumTestContexts];
  v8::HeapStatistics heap_statistics;
  CHECK_EQ(0u, heap_statistics.number_of_native_contexts());
  CcTest::isolate()->GetHeapStatistics(&heap_statistics);
  CHECK_EQ(0u, heap_statistics.number_of_native_contexts());
  for (size_t i = 0; i < kNumTestContexts; i++) {
    i::HandleScope inner(isolate);
    context[i].Reset(CcTest::isolate(), v8::Context::New(CcTest::isolate()));
    CcTest::isolate()->GetHeapStatistics(&heap_statistics);
    CHECK_EQ(i + 1, heap_statistics.number_of_native_contexts());
  }
  for (size_t i = 0; i < kNumTestContexts; i++) {
    context[i].Reset();
    CcTest::PreciseCollectAllGarbage();
    CcTest::isolate()->GetHeapStatistics(&heap_statistics);
    CHECK_EQ(kNumTestContexts - i - 1u,
             heap_statistics.number_of_native_contexts());
  }
}

TEST(NumberOfDetachedContexts) {
  static const size_t kNumTestContexts = 10;
  i::Isolate* isolate = CcTest::i_isolate();
  i::HandleScope scope(isolate);
  v8::Global<v8::Context> context[kNumTestContexts];
  v8::HeapStatistics heap_statistics;
  CHECK_EQ(0u, heap_statistics.number_of_detached_contexts());
  CcTest::isolate()->GetHeapStatistics(&heap_statistics);
  CHECK_EQ(0u, heap_statistics.number_of_detached_contexts());
  for (size_t i = 0; i < kNumTestContexts; i++) {
    i::HandleScope inner(isolate);
    v8::Local<v8::Context> local = v8::Context::New(CcTest::isolate());
    context[i].Reset(CcTest::isolate(), local);
    local->DetachGlobal();
    CcTest::isolate()->GetHeapStatistics(&heap_statistics);
    CHECK_EQ(i + 1, heap_statistics.number_of_detached_contexts());
  }
  for (size_t i = 0; i < kNumTestContexts; i++) {
    context[i].Reset();
    CcTest::PreciseCollectAllGarbage();
    CcTest::isolate()->GetHeapStatistics(&heap_statistics);
    CHECK_EQ(kNumTestContexts - i - 1u,
             heap_statistics.number_of_detached_contexts());
  }
}

class VisitorImpl : public v8::ExternalResourceVisitor {
 public:
  explicit VisitorImpl(TestResource** resource) {
    for (int i = 0; i < 4; i++) {
      resource_[i] = resource[i];
      found_resource_[i] = false;
    }
  }
  ~VisitorImpl() override = default;
  void VisitExternalString(v8::Local<v8::String> string) override {
    if (!string->IsExternal()) {
      CHECK(string->IsExternalOneByte());
      return;
    }
    v8::String::ExternalStringResource* resource =
        string->GetExternalStringResource();
    CHECK(resource);
    for (int i = 0; i < 4; i++) {
      if (resource_[i] == resource) {
        CHECK(!found_resource_[i]);
        found_resource_[i] = true;
      }
    }
  }
  void CheckVisitedResources() {
    for (int i = 0; i < 4; i++) {
      CHECK(found_resource_[i]);
    }
  }

 private:
  v8::String::ExternalStringResource* resource_[4];
  bool found_resource_[4];
};


TEST(ExternalizeOldSpaceTwoByteCons) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> cons =
      CompileRun("'Romeo Montague ' + 'Juliet Capulet'")
          ->ToString(env.local())
          .ToLocalChecked();
  CHECK(v8::Utils::OpenHandle(*cons)->IsConsString());
  CcTest::CollectAllAvailableGarbage();
  CHECK(CcTest::heap()->old_space()->Contains(*v8::Utils::OpenHandle(*cons)));

  TestResource* resource = new TestResource(
      AsciiToTwoByteString("Romeo Montague Juliet Capulet"));
  cons->MakeExternal(resource);

  CHECK(cons->IsExternal());
  CHECK_EQ(resource, cons->GetExternalStringResource());
  String::Encoding encoding;
  CHECK_EQ(resource, cons->GetExternalStringResourceBase(&encoding));
  CHECK_EQ(String::TWO_BYTE_ENCODING, encoding);
}


TEST(ExternalizeOldSpaceOneByteCons) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> cons =
      CompileRun("'Romeo Montague ' + 'Juliet Capulet'")
          ->ToString(env.local())
          .ToLocalChecked();
  CHECK(v8::Utils::OpenHandle(*cons)->IsConsString());
  CcTest::CollectAllAvailableGarbage();
  CHECK(CcTest::heap()->old_space()->Contains(*v8::Utils::OpenHandle(*cons)));

  TestOneByteResource* resource =
      new TestOneByteResource(i::StrDup("Romeo Montague Juliet Capulet"));
  cons->MakeExternal(resource);

  CHECK(cons->IsExternalOneByte());
  CHECK_EQ(resource, cons->GetExternalOneByteStringResource());
  String::Encoding encoding;
  CHECK_EQ(resource, cons->GetExternalStringResourceBase(&encoding));
  CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
}


TEST(VisitExternalStrings) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  const char* string = "Some string";
  uint16_t* two_byte_string = AsciiToTwoByteString(string);
  TestResource* resource[4];
  resource[0] = new TestResource(two_byte_string);
  v8::Local<v8::String> string0 =
      v8::String::NewExternalTwoByte(env->GetIsolate(), resource[0])
          .ToLocalChecked();
  resource[1] = new TestResource(two_byte_string, nullptr, false);
  v8::Local<v8::String> string1 =
      v8::String::NewExternalTwoByte(env->GetIsolate(), resource[1])
          .ToLocalChecked();

  // Externalized symbol.
  resource[2] = new TestResource(two_byte_string, nullptr, false);
  v8::Local<v8::String> string2 =
      v8::String::NewFromUtf8(env->GetIsolate(), string,
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  CHECK(string2->MakeExternal(resource[2]));

  // Symbolized External.
  resource[3] = new TestResource(AsciiToTwoByteString("Some other string"));
  v8::Local<v8::String> string3 =
      v8::String::NewExternalTwoByte(env->GetIsolate(), resource[3])
          .ToLocalChecked();
  CcTest::CollectAllAvailableGarbage();  // Tenure string.
  // Turn into a symbol.
  i::Handle<i::String> string3_i = v8::Utils::OpenHandle(*string3);
  CHECK(!CcTest::i_isolate()->factory()->InternalizeString(
      string3_i).is_null());
  CHECK(string3_i->IsInternalizedString());

  // We need to add usages for string* to avoid warnings in GCC 4.7
  CHECK(string0->IsExternal());
  CHECK(string1->IsExternal());
  CHECK(string2->IsExternal());
  CHECK(string3->IsExternal());

  VisitorImpl visitor(resource);
  isolate->VisitExternalResources(&visitor);
  visitor.CheckVisitedResources();
}


TEST(ExternalStringCollectedAtTearDown) {
  int destroyed = 0;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  { v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    const char* s = "One string to test them all, one string to find them.";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring =
        v8::String::NewExternalOneByte(isolate, inscription).ToLocalChecked();
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  isolate->Dispose();
  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


TEST(ExternalInternalizedStringCollectedAtTearDown) {
  int destroyed = 0;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  { v8::Isolate::Scope isolate_scope(isolate);
    LocalContext env(isolate);
    v8::HandleScope handle_scope(isolate);
    CompileRun("var ring = 'One string to test them all';");
    const char* s = "One string to test them all";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring =
        CompileRun("ring")->ToString(env.local()).ToLocalChecked();
    CHECK(v8::Utils::OpenHandle(*ring)->IsInternalizedString());
    ring->MakeExternal(inscription);
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  isolate->Dispose();
  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


TEST(ExternalInternalizedStringCollectedAtGC) {
  int destroyed = 0;
  { LocalContext env;
    v8::HandleScope handle_scope(env->GetIsolate());
    CompileRun("var ring = 'One string to test them all';");
    const char* s = "One string to test them all";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring = CompileRun("ring").As<v8::String>();
    CHECK(v8::Utils::OpenHandle(*ring)->IsInternalizedString());
    ring->MakeExternal(inscription);
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  // Garbage collector deals swift blows to evil.
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::CollectAllAvailableGarbage();

  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


static double DoubleFromBits(uint64_t value) {
  double target;
  i::MemCopy(&target, &value, sizeof(target));
  return target;
}


static uint64_t DoubleToBits(double value) {
  uint64_t target;
  i::MemCopy(&target, &value, sizeof(target));
  return target;
}


static double DoubleToDateTime(double input) {
  double date_limit = 864e13;
  if (std::isnan(input) || input < -date_limit || input > date_limit) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return (input < 0) ? -(std::floor(-input)) : std::floor(input);
}


// We don't have a consistent way to write 64-bit constants syntactically, so we
// split them into two 32-bit constants and combine them programmatically.
static double DoubleFromBits(uint32_t high_bits, uint32_t low_bits) {
  return DoubleFromBits((static_cast<uint64_t>(high_bits) << 32) | low_bits);
}


THREADED_TEST(QuietSignalingNaNs) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);

  // Special double values.
  double snan = DoubleFromBits(0x7FF00000, 0x00000001);
  double qnan = DoubleFromBits(0x7FF80000, 0x00000000);
  double infinity = DoubleFromBits(0x7FF00000, 0x00000000);
  double max_normal = DoubleFromBits(0x7FEFFFFF, 0xFFFFFFFFu);
  double min_normal = DoubleFromBits(0x00100000, 0x00000000);
  double max_denormal = DoubleFromBits(0x000FFFFF, 0xFFFFFFFFu);
  double min_denormal = DoubleFromBits(0x00000000, 0x00000001);

  // Date values are capped at +/-100000000 days (times 864e5 ms per day)
  // on either side of the epoch.
  double date_limit = 864e13;

  double test_values[] = {
      snan,
      qnan,
      infinity,
      max_normal,
      date_limit + 1,
      date_limit,
      min_normal,
      max_denormal,
      min_denormal,
      0,
      -0,
      -min_denormal,
      -max_denormal,
      -min_normal,
      -date_limit,
      -date_limit - 1,
      -max_normal,
      -infinity,
      -qnan,
      -snan
  };
  int num_test_values = 20;

  for (int i = 0; i < num_test_values; i++) {
    double test_value = test_values[i];

    // Check that Number::New preserves non-NaNs and quiets SNaNs.
    v8::Local<v8::Value> number = v8::Number::New(isolate, test_value);
    double stored_number = number->NumberValue(context.local()).FromJust();
    if (!std::isnan(test_value)) {
      CHECK_EQ(test_value, stored_number);
    } else {
      uint64_t stored_bits = DoubleToBits(stored_number);
      // Check if quiet nan (bits 51..62 all set).
#if (defined(V8_TARGET_ARCH_MIPS) || defined(V8_TARGET_ARCH_MIPS64)) && \
    !defined(_MIPS_ARCH_MIPS64R6) && !defined(_MIPS_ARCH_MIPS32R6) &&   \
    !defined(USE_SIMULATOR)
      // Most significant fraction bit for quiet nan is set to 0
      // on MIPS architecture. Allowed by IEEE-754.
      CHECK_EQ(0xFFE, static_cast<int>((stored_bits >> 51) & 0xFFF));
#else
      CHECK_EQ(0xFFF, static_cast<int>((stored_bits >> 51) & 0xFFF));
#endif
    }

    // Check that Date::New preserves non-NaNs in the date range and
    // quiets SNaNs.
    v8::Local<v8::Value> date =
        v8::Date::New(context.local(), test_value).ToLocalChecked();
    double expected_stored_date = DoubleToDateTime(test_value);
    double stored_date = date->NumberValue(context.local()).FromJust();
    if (!std::isnan(expected_stored_date)) {
      CHECK_EQ(expected_stored_date, stored_date);
    } else {
      uint64_t stored_bits = DoubleToBits(stored_date);
      // Check if quiet nan (bits 51..62 all set).
#if (defined(V8_TARGET_ARCH_MIPS) || defined(V8_TARGET_ARCH_MIPS64)) && \
    !defined(_MIPS_ARCH_MIPS64R6) && !defined(_MIPS_ARCH_MIPS32R6) &&   \
    !defined(USE_SIMULATOR)
      // Most significant fraction bit for quiet nan is set to 0
      // on MIPS architecture. Allowed by IEEE-754.
      CHECK_EQ(0xFFE, static_cast<int>((stored_bits >> 51) & 0xFFF));
#else
      CHECK_EQ(0xFFF, static_cast<int>((stored_bits >> 51) & 0xFFF));
#endif
    }
  }
}


static void SpaghettiIncident(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::TryCatch tc(args.GetIsolate());
  v8::MaybeLocal<v8::String> str(
      args[0]->ToString(args.GetIsolate()->GetCurrentContext()));
  USE(str);
  if (tc.HasCaught())
    tc.ReThrow();
}


// Test that an exception can be propagated down through a spaghetti
// stack using ReThrow.
THREADED_TEST(SpaghettiStackReThrow) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  context->Global()
      ->Set(context.local(), v8_str("s"),
            v8::FunctionTemplate::New(isolate, SpaghettiIncident)
                ->GetFunction(context.local())
                .ToLocalChecked())
      .FromJust();
  v8::TryCatch try_catch(isolate);
  CompileRun(
      "var i = 0;"
      "var o = {"
      "  toString: function () {"
      "    if (i == 10) {"
      "      throw 'Hey!';"
      "    } else {"
      "      i++;"
      "      return s(o);"
      "    }"
      "  }"
      "};"
      "s(o);");
  CHECK(try_catch.HasCaught());
  v8::String::Utf8Value value(isolate, try_catch.Exception());
  CHECK_EQ(0, strcmp(*value, "Hey!"));
}


TEST(Regress528) {
  ManualGCScope manual_gc_scope;
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  i::FLAG_retain_maps_for_n_gc = 0;
  v8::HandleScope scope(isolate);
  v8::Local<Context> other_context;
  int gc_count;

  // Create a context used to keep the code from aging in the compilation
  // cache.
  other_context = Context::New(isolate);

  // Context-dependent context data creates reference from the compilation
  // cache to the global object.
  const char* source_simple = "1";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    Local<v8::String> obj = v8_str("");
    context->SetEmbedderData(0, obj);
    CompileRun(source_simple);
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_simple);
    other_context->Exit();
    CcTest::CollectAllGarbage();
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  // Eval in a function creates reference from the compilation cache to the
  // global object.
  const char* source_eval = "function f(){eval('1')}; f()";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    CompileRun(source_eval);
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_eval);
    other_context->Exit();
    CcTest::CollectAllGarbage();
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  // Looking up the line number for an exception creates reference from the
  // compilation cache to the global object.
  const char* source_exception = "function f(){throw 1;} f()";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    v8::TryCatch try_catch(isolate);
    CompileRun(source_exception);
    CHECK(try_catch.HasCaught());
    v8::Local<v8::Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(1, message->GetLineNumber(context).FromJust());
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_exception);
    other_context->Exit();
    CcTest::CollectAllGarbage();
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  isolate->ContextDisposedNotification();
}


THREADED_TEST(ScriptOrigin) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<v8::PrimitiveArray> array(v8::PrimitiveArray::New(isolate, 1));
  Local<v8::Symbol> symbol(v8::Symbol::New(isolate));
  array->Set(isolate, 0, symbol);

  v8::ScriptOrigin origin = v8::ScriptOrigin(
      v8_str("test"), v8::Integer::New(env->GetIsolate(), 1),
      v8::Integer::New(env->GetIsolate(), 1), v8::True(env->GetIsolate()),
      v8::Local<v8::Integer>(), v8_str("http://sourceMapUrl"),
      v8::True(env->GetIsolate()), v8::False(env->GetIsolate()),
      v8::False(env->GetIsolate()), array);
  v8::Local<v8::String> script = v8_str("function f() {}\n\nfunction g() {}");
  v8::Script::Compile(env.local(), script, &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("f")).ToLocalChecked());
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("g")).ToLocalChecked());

  v8::ScriptOrigin script_origin_f = f->GetScriptOrigin();
  CHECK_EQ(0, strcmp("test",
                     *v8::String::Utf8Value(env->GetIsolate(),
                                            script_origin_f.ResourceName())));
  CHECK_EQ(
      1,
      script_origin_f.ResourceLineOffset()->Int32Value(env.local()).FromJust());
  CHECK(script_origin_f.Options().IsSharedCrossOrigin());
  CHECK(script_origin_f.Options().IsOpaque());
  printf("is name = %d\n", script_origin_f.SourceMapUrl()->IsUndefined());
  CHECK(script_origin_f.HostDefinedOptions()->Get(isolate, 0)->IsSymbol());

  CHECK_EQ(0, strcmp("http://sourceMapUrl",
                     *v8::String::Utf8Value(env->GetIsolate(),
                                            script_origin_f.SourceMapUrl())));

  v8::ScriptOrigin script_origin_g = g->GetScriptOrigin();
  CHECK_EQ(0, strcmp("test",
                     *v8::String::Utf8Value(env->GetIsolate(),
                                            script_origin_g.ResourceName())));
  CHECK_EQ(
      1,
      script_origin_g.ResourceLineOffset()->Int32Value(env.local()).FromJust());
  CHECK(script_origin_g.Options().IsSharedCrossOrigin());
  CHECK(script_origin_g.Options().IsOpaque());
  CHECK_EQ(0, strcmp("http://sourceMapUrl",
                     *v8::String::Utf8Value(env->GetIsolate(),
                                            script_origin_g.SourceMapUrl())));
  CHECK(script_origin_g.HostDefinedOptions()->Get(isolate, 0)->IsSymbol());
}


THREADED_TEST(FunctionGetInferredName) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8_str("test"));
  v8::Local<v8::String> script =
      v8_str("var foo = { bar : { baz : function() {}}}; var f = foo.bar.baz;");
  v8::Script::Compile(env.local(), script, &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("f")).ToLocalChecked());
  CHECK_EQ(0,
           strcmp("foo.bar.baz", *v8::String::Utf8Value(env->GetIsolate(),
                                                        f->GetInferredName())));
}


THREADED_TEST(FunctionGetDebugName) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  const char* code =
      "var error = false;"
      "function a() { this.x = 1; };"
      "a.displayName = 'display_a';"
      "var b = (function() {"
      "  var f = function() { this.x = 2; };"
      "  f.displayName = 'display_b';"
      "  return f;"
      "})();"
      "var c = function() {};"
      "c.__defineGetter__('displayName', function() {"
      "  error = true;"
      "  throw new Error();"
      "});"
      "function d() {};"
      "d.__defineGetter__('displayName', function() {"
      "  error = true;"
      "  return 'wrong_display_name';"
      "});"
      "function e() {};"
      "e.displayName = 'wrong_display_name';"
      "e.__defineSetter__('displayName', function() {"
      "  error = true;"
      "  throw new Error();"
      "});"
      "function f() {};"
      "f.displayName = { 'foo': 6, toString: function() {"
      "  error = true;"
      "  return 'wrong_display_name';"
      "}};"
      "var g = function() {"
      "  arguments.callee.displayName = 'set_in_runtime';"
      "}; g();"
      "var h = function() {};"
      "h.displayName = 'displayName';"
      "Object.defineProperty(h, 'name', { value: 'function.name' });"
      "var i = function() {};"
      "i.displayName = 239;"
      "Object.defineProperty(i, 'name', { value: 'function.name' });"
      "var j = function() {};"
      "Object.defineProperty(j, 'name', { value: 'function.name' });"
      "var foo = { bar : { baz : (0, function() {})}}; var k = foo.bar.baz;"
      "var foo = { bar : { baz : function() {} }}; var l = foo.bar.baz;";
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8_str("test"));
  v8::Script::Compile(env.local(), v8_str(code), &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Value> error =
      env->Global()->Get(env.local(), v8_str("error")).ToLocalChecked();
  CHECK(!error->BooleanValue(isolate));
  const char* functions[] = {"a", "display_a",
                             "b", "display_b",
                             "c", "c",
                             "d", "d",
                             "e", "e",
                             "f", "f",
                             "g", "set_in_runtime",
                             "h", "displayName",
                             "i", "function.name",
                             "j", "function.name",
                             "k", "foo.bar.baz",
                             "l", "baz"};
  for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]) / 2; ++i) {
    v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
        env->Global()
            ->Get(env.local(),
                  v8::String::NewFromUtf8(isolate, functions[i * 2],
                                          v8::NewStringType::kNormal)
                      .ToLocalChecked())
            .ToLocalChecked());
    CHECK_EQ(0, strcmp(functions[i * 2 + 1],
                       *v8::String::Utf8Value(isolate, f->GetDebugName())));
  }
}


THREADED_TEST(FunctionGetDisplayName) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  const char* code = "var error = false;"
                     "function a() { this.x = 1; };"
                     "a.displayName = 'display_a';"
                     "var b = (function() {"
                     "  var f = function() { this.x = 2; };"
                     "  f.displayName = 'display_b';"
                     "  return f;"
                     "})();"
                     "var c = function() {};"
                     "c.__defineGetter__('displayName', function() {"
                     "  error = true;"
                     "  throw new Error();"
                     "});"
                     "function d() {};"
                     "d.__defineGetter__('displayName', function() {"
                     "  error = true;"
                     "  return 'wrong_display_name';"
                     "});"
                     "function e() {};"
                     "e.displayName = 'wrong_display_name';"
                     "e.__defineSetter__('displayName', function() {"
                     "  error = true;"
                     "  throw new Error();"
                     "});"
                     "function f() {};"
                     "f.displayName = { 'foo': 6, toString: function() {"
                     "  error = true;"
                     "  return 'wrong_display_name';"
                     "}};"
                     "var g = function() {"
                     "  arguments.callee.displayName = 'set_in_runtime';"
                     "}; g();";
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8_str("test"));
  v8::Script::Compile(env.local(), v8_str(code), &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Value> error =
      env->Global()->Get(env.local(), v8_str("error")).ToLocalChecked();
  v8::Local<v8::Function> a = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("a")).ToLocalChecked());
  v8::Local<v8::Function> b = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("b")).ToLocalChecked());
  v8::Local<v8::Function> c = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("c")).ToLocalChecked());
  v8::Local<v8::Function> d = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("d")).ToLocalChecked());
  v8::Local<v8::Function> e = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("e")).ToLocalChecked());
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("f")).ToLocalChecked());
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("g")).ToLocalChecked());
  CHECK(!error->BooleanValue(isolate));
  CHECK_EQ(0, strcmp("display_a",
                     *v8::String::Utf8Value(isolate, a->GetDisplayName())));
  CHECK_EQ(0, strcmp("display_b",
                     *v8::String::Utf8Value(isolate, b->GetDisplayName())));
  CHECK(c->GetDisplayName()->IsUndefined());
  CHECK(d->GetDisplayName()->IsUndefined());
  CHECK(e->GetDisplayName()->IsUndefined());
  CHECK(f->GetDisplayName()->IsUndefined());
  CHECK_EQ(0, strcmp("set_in_runtime",
                     *v8::String::Utf8Value(isolate, g->GetDisplayName())));
}


THREADED_TEST(ScriptLineNumber) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8_str("test"));
  v8::Local<v8::String> script = v8_str("function f() {}\n\nfunction g() {}");
  v8::Script::Compile(env.local(), script, &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("f")).ToLocalChecked());
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("g")).ToLocalChecked());
  CHECK_EQ(0, f->GetScriptLineNumber());
  CHECK_EQ(2, g->GetScriptLineNumber());
}


THREADED_TEST(ScriptColumnNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("test"), v8::Integer::New(isolate, 3),
                       v8::Integer::New(isolate, 2));
  v8::Local<v8::String> script =
      v8_str("function foo() {}\n\n     function bar() {}");
  v8::Script::Compile(env.local(), script, &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Function> foo = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("foo")).ToLocalChecked());
  v8::Local<v8::Function> bar = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("bar")).ToLocalChecked());
  CHECK_EQ(14, foo->GetScriptColumnNumber());
  CHECK_EQ(17, bar->GetScriptColumnNumber());
}


THREADED_TEST(FunctionGetScriptId) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("test"), v8::Integer::New(isolate, 3),
                       v8::Integer::New(isolate, 2));
  v8::Local<v8::String> scriptSource =
      v8_str("function foo() {}\n\n     function bar() {}");
  v8::Local<v8::Script> script(
      v8::Script::Compile(env.local(), scriptSource, &origin).ToLocalChecked());
  script->Run(env.local()).ToLocalChecked();
  v8::Local<v8::Function> foo = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("foo")).ToLocalChecked());
  v8::Local<v8::Function> bar = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("bar")).ToLocalChecked());
  CHECK_EQ(script->GetUnboundScript()->GetId(), foo->ScriptId());
  CHECK_EQ(script->GetUnboundScript()->GetId(), bar->ScriptId());
}


THREADED_TEST(FunctionGetBoundFunction) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8_str("test"));
  v8::Local<v8::String> script = v8_str(
      "var a = new Object();\n"
      "a.x = 1;\n"
      "function f () { return this.x };\n"
      "var g = f.bind(a);\n"
      "var b = g();");
  v8::Script::Compile(env.local(), script, &origin)
      .ToLocalChecked()
      ->Run(env.local())
      .ToLocalChecked();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("f")).ToLocalChecked());
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(env.local(), v8_str("g")).ToLocalChecked());
  CHECK(g->GetBoundFunction()->IsFunction());
  Local<v8::Function> original_function = Local<v8::Function>::Cast(
      g->GetBoundFunction());
  CHECK(f->GetName()
            ->Equals(env.local(), original_function->GetName())
            .FromJust());
  CHECK_EQ(f->GetScriptLineNumber(), original_function->GetScriptLineNumber());
  CHECK_EQ(f->GetScriptColumnNumber(),
           original_function->GetScriptColumnNumber());
}


static void GetterWhichReturns42(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  info.GetReturnValue().Set(v8_num(42));
}


static void SetterWhichSetsYOnThisTo23(
    Local<String> name,
    Local<Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  Local<Object>::Cast(info.This())
      ->Set(info.GetIsolate()->GetCurrentContext(), v8_str("y"), v8_num(23))
      .FromJust();
}


void FooGetInterceptor(Local<Name> name,
                       const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  if (!name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
           .FromJust()) {
    return;
  }
  info.GetReturnValue().Set(v8_num(42));
}


void FooSetInterceptor(Local<Name> name, Local<Value> value,
                       const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  if (!name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
           .FromJust()) {
    return;
  }
  Local<Object>::Cast(info.This())
      ->Set(info.GetIsolate()->GetCurrentContext(), v8_str("y"), v8_num(23))
      .FromJust();
  info.GetReturnValue().Set(v8_num(23));
}


TEST(SetterOnConstructorPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("P"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("function C1() {"
             "  this.x = 23;"
             "};"
             "C1.prototype = P;"
             "function C2() {"
             "  this.x = 23"
             "};"
             "C2.prototype = { };"
             "C2.prototype.__proto__ = P;");

  v8::Local<v8::Script> script;
  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Local<v8::Object> c1 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(42, c1->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(23, c1->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }

  script = v8_compile("new C2();");
  for (int i = 0; i < 10; i++) {
    v8::Local<v8::Object> c2 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(42, c2->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(23, c2->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }
}


static void NamedPropertySetterWhichSetsYOnThisTo23(
    Local<Name> name, Local<Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("x"))
          .FromJust()) {
    Local<Object>::Cast(info.This())
        ->Set(info.GetIsolate()->GetCurrentContext(), v8_str("y"), v8_num(23))
        .FromJust();
  }
}


THREADED_TEST(InterceptorOnConstructorPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      NamedPropertyGetterWhichReturns42,
      NamedPropertySetterWhichSetsYOnThisTo23));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("P"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("function C1() {"
             "  this.x = 23;"
             "};"
             "C1.prototype = P;"
             "function C2() {"
             "  this.x = 23"
             "};"
             "C2.prototype = { };"
             "C2.prototype.__proto__ = P;");

  v8::Local<v8::Script> script;
  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Local<v8::Object> c1 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(23, c1->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(42, c1->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }

  script = v8_compile("new C2();");
  for (int i = 0; i < 10; i++) {
    v8::Local<v8::Object> c2 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(23, c2->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(42, c2->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }
}


TEST(Regress618) {
  const char* source = "function C1() {"
                       "  this.x = 23;"
                       "};"
                       "C1.prototype = P;";

  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Script> script;

  // Use a simple object as prototype.
  v8::Local<v8::Object> prototype = v8::Object::New(isolate);
  prototype->Set(context.local(), v8_str("y"), v8_num(42)).FromJust();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("P"), prototype)
            .FromJust());

  // This compile will add the code to the compilation cache.
  CompileRun(source);

  script = v8_compile("new C1();");
  // Allow enough iterations for the inobject slack tracking logic
  // to finalize instance size and install the fast construct stub.
  for (int i = 0; i < 256; i++) {
    v8::Local<v8::Object> c1 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(23, c1->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(42, c1->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }

  // Use an API object with accessors as prototype.
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("P"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  // This compile will get the code from the compilation cache.
  CompileRun(source);

  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Local<v8::Object> c1 = v8::Local<v8::Object>::Cast(
        script->Run(context.local()).ToLocalChecked());
    CHECK_EQ(42, c1->Get(context.local(), v8_str("x"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
    CHECK_EQ(23, c1->Get(context.local(), v8_str("y"))
                     .ToLocalChecked()
                     ->Int32Value(context.local())
                     .FromJust());
  }
}

v8::Isolate* gc_callbacks_isolate = nullptr;
int prologue_call_count = 0;
int epilogue_call_count = 0;
int prologue_call_count_second = 0;
int epilogue_call_count_second = 0;
int prologue_call_count_alloc = 0;
int epilogue_call_count_alloc = 0;

void PrologueCallback(v8::Isolate* isolate,
                      v8::GCType,
                      v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count;
}

void EpilogueCallback(v8::Isolate* isolate,
                      v8::GCType,
                      v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count;
}


void PrologueCallbackSecond(v8::Isolate* isolate,
                            v8::GCType,
                            v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count_second;
}


void EpilogueCallbackSecond(v8::Isolate* isolate,
                            v8::GCType,
                            v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count_second;
}

void PrologueCallbackNew(v8::Isolate* isolate, v8::GCType,
                         v8::GCCallbackFlags flags, void* data) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++*static_cast<int*>(data);
}

void EpilogueCallbackNew(v8::Isolate* isolate, v8::GCType,
                         v8::GCCallbackFlags flags, void* data) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++*static_cast<int*>(data);
}

void PrologueCallbackAlloc(v8::Isolate* isolate,
                           v8::GCType,
                           v8::GCCallbackFlags flags) {
  v8::HandleScope scope(isolate);

  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count_alloc;

  // Simulate full heap to see if we will reenter this callback
  i::heap::SimulateFullSpace(CcTest::heap()->new_space());

  Local<Object> obj = Object::New(isolate);
  CHECK(!obj.IsEmpty());

  CcTest::PreciseCollectAllGarbage();
}


void EpilogueCallbackAlloc(v8::Isolate* isolate,
                           v8::GCType,
                           v8::GCCallbackFlags flags) {
  v8::HandleScope scope(isolate);

  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count_alloc;

  // Simulate full heap to see if we will reenter this callback
  i::heap::SimulateFullSpace(CcTest::heap()->new_space());

  Local<Object> obj = Object::New(isolate);
  CHECK(!obj.IsEmpty());

  CcTest::PreciseCollectAllGarbage();
}


TEST(GCCallbacksOld) {
  LocalContext context;

  gc_callbacks_isolate = context->GetIsolate();

  context->GetIsolate()->AddGCPrologueCallback(PrologueCallback);
  context->GetIsolate()->AddGCEpilogueCallback(EpilogueCallback);
  CHECK_EQ(0, prologue_call_count);
  CHECK_EQ(0, epilogue_call_count);
  CcTest::CollectAllGarbage();
  CHECK_EQ(1, prologue_call_count);
  CHECK_EQ(1, epilogue_call_count);
  context->GetIsolate()->AddGCPrologueCallback(PrologueCallbackSecond);
  context->GetIsolate()->AddGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(1, prologue_call_count_second);
  CHECK_EQ(1, epilogue_call_count_second);
  context->GetIsolate()->RemoveGCPrologueCallback(PrologueCallback);
  context->GetIsolate()->RemoveGCEpilogueCallback(EpilogueCallback);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
  context->GetIsolate()->RemoveGCPrologueCallback(PrologueCallbackSecond);
  context->GetIsolate()->RemoveGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
}

TEST(GCCallbacksWithData) {
  LocalContext context;

  gc_callbacks_isolate = context->GetIsolate();
  int prologue1 = 0;
  int epilogue1 = 0;
  int prologue2 = 0;
  int epilogue2 = 0;

  context->GetIsolate()->AddGCPrologueCallback(PrologueCallbackNew, &prologue1);
  context->GetIsolate()->AddGCEpilogueCallback(EpilogueCallbackNew, &epilogue1);
  CHECK_EQ(0, prologue1);
  CHECK_EQ(0, epilogue1);
  CHECK_EQ(0, prologue2);
  CHECK_EQ(0, epilogue2);
  CcTest::CollectAllGarbage();
  CHECK_EQ(1, prologue1);
  CHECK_EQ(1, epilogue1);
  CHECK_EQ(0, prologue2);
  CHECK_EQ(0, epilogue2);
  context->GetIsolate()->AddGCPrologueCallback(PrologueCallbackNew, &prologue2);
  context->GetIsolate()->AddGCEpilogueCallback(EpilogueCallbackNew, &epilogue2);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue1);
  CHECK_EQ(2, epilogue1);
  CHECK_EQ(1, prologue2);
  CHECK_EQ(1, epilogue2);
  context->GetIsolate()->RemoveGCPrologueCallback(PrologueCallbackNew,
                                                  &prologue1);
  context->GetIsolate()->RemoveGCEpilogueCallback(EpilogueCallbackNew,
                                                  &epilogue1);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue1);
  CHECK_EQ(2, epilogue1);
  CHECK_EQ(2, prologue2);
  CHECK_EQ(2, epilogue2);
  context->GetIsolate()->RemoveGCPrologueCallback(PrologueCallbackNew,
                                                  &prologue2);
  context->GetIsolate()->RemoveGCEpilogueCallback(EpilogueCallbackNew,
                                                  &epilogue2);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue1);
  CHECK_EQ(2, epilogue1);
  CHECK_EQ(2, prologue2);
  CHECK_EQ(2, epilogue2);
}

TEST(GCCallbacks) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  gc_callbacks_isolate = isolate;
  isolate->AddGCPrologueCallback(PrologueCallback);
  isolate->AddGCEpilogueCallback(EpilogueCallback);
  CHECK_EQ(0, prologue_call_count);
  CHECK_EQ(0, epilogue_call_count);
  CcTest::CollectAllGarbage();
  CHECK_EQ(1, prologue_call_count);
  CHECK_EQ(1, epilogue_call_count);
  isolate->AddGCPrologueCallback(PrologueCallbackSecond);
  isolate->AddGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(1, prologue_call_count_second);
  CHECK_EQ(1, epilogue_call_count_second);
  isolate->RemoveGCPrologueCallback(PrologueCallback);
  isolate->RemoveGCEpilogueCallback(EpilogueCallback);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
  isolate->RemoveGCPrologueCallback(PrologueCallbackSecond);
  isolate->RemoveGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::CollectAllGarbage();
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);

  CHECK_EQ(0, prologue_call_count_alloc);
  CHECK_EQ(0, epilogue_call_count_alloc);
  isolate->AddGCPrologueCallback(PrologueCallbackAlloc);
  isolate->AddGCEpilogueCallback(EpilogueCallbackAlloc);
  CcTest::PreciseCollectAllGarbage();
  CHECK_EQ(1, prologue_call_count_alloc);
  CHECK_EQ(1, epilogue_call_count_alloc);
  isolate->RemoveGCPrologueCallback(PrologueCallbackAlloc);
  isolate->RemoveGCEpilogueCallback(EpilogueCallbackAlloc);
}


THREADED_TEST(TwoByteStringInOneByteCons) {
  // See Chromium issue 47824.
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* init_code =
      "var str1 = 'abelspendabel';"
      "var str2 = str1 + str1 + str1;"
      "str2;";
  Local<Value> result = CompileRun(init_code);

  Local<Value> indexof = CompileRun("str2.indexOf('els')");
  Local<Value> lastindexof = CompileRun("str2.lastIndexOf('dab')");

  CHECK(result->IsString());
  i::Handle<i::String> string = v8::Utils::OpenHandle(String::Cast(*result));
  int length = string->length();
  CHECK(string->IsOneByteRepresentation());

  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(context->GetIsolate());
  i::Handle<i::String> flat_string = i::String::Flatten(i_isolate, string);

  CHECK(string->IsOneByteRepresentation());
  CHECK(flat_string->IsOneByteRepresentation());

  // Create external resource.
  uint16_t* uc16_buffer = new uint16_t[length + 1];

  i::String::WriteToFlat(*flat_string, uc16_buffer, 0, length);
  uc16_buffer[length] = 0;

  TestResource resource(uc16_buffer);

  flat_string->MakeExternal(&resource);

  CHECK(flat_string->IsTwoByteRepresentation());

  // If the cons string has been short-circuited, skip the following checks.
  if (!string.is_identical_to(flat_string)) {
    // At this point, we should have a Cons string which is flat and one-byte,
    // with a first half that is a two-byte string (although it only contains
    // one-byte characters). This is a valid sequence of steps, and it can
    // happen in real pages.
    CHECK(string->IsOneByteRepresentation());
    i::ConsString cons = i::ConsString::cast(*string);
    CHECK_EQ(0, cons.second().length());
    CHECK(cons.first().IsTwoByteRepresentation());
  }

  // Check that some string operations work.

  // Atom RegExp.
  Local<Value> reresult = CompileRun("str2.match(/abel/g).length;");
  CHECK_EQ(6, reresult->Int32Value(context.local()).FromJust());

  // Nonatom RegExp.
  reresult = CompileRun("str2.match(/abe./g).length;");
  CHECK_EQ(6, reresult->Int32Value(context.local()).FromJust());

  reresult = CompileRun("str2.search(/bel/g);");
  CHECK_EQ(1, reresult->Int32Value(context.local()).FromJust());

  reresult = CompileRun("str2.search(/be./g);");
  CHECK_EQ(1, reresult->Int32Value(context.local()).FromJust());

  ExpectTrue("/bel/g.test(str2);");

  ExpectTrue("/be./g.test(str2);");

  reresult = CompileRun("/bel/g.exec(str2);");
  CHECK(!reresult->IsNull());

  reresult = CompileRun("/be./g.exec(str2);");
  CHECK(!reresult->IsNull());

  ExpectString("str2.substring(2, 10);", "elspenda");

  ExpectString("str2.substring(2, 20);", "elspendabelabelspe");

  ExpectString("str2.charAt(2);", "e");

  ExpectObject("str2.indexOf('els');", indexof);

  ExpectObject("str2.lastIndexOf('dab');", lastindexof);

  reresult = CompileRun("str2.charCodeAt(2);");
  CHECK_EQ(static_cast<int32_t>('e'),
           reresult->Int32Value(context.local()).FromJust());
  // This avoids the GC from trying to free stack allocated resources.
  i::Handle<i::ExternalTwoByteString>::cast(flat_string)
      ->SetResource(i_isolate, nullptr);
}


TEST(ContainsOnlyOneByte) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  // Make a buffer long enough that it won't automatically be converted.
  const int length = 512;
  // Ensure word aligned assignment.
  const int aligned_length = length*sizeof(uintptr_t)/sizeof(uint16_t);
  std::unique_ptr<uintptr_t[]> aligned_contents(new uintptr_t[aligned_length]);
  uint16_t* string_contents =
      reinterpret_cast<uint16_t*>(aligned_contents.get());
  // Set to contain only one byte.
  for (int i = 0; i < length-1; i++) {
    string_contents[i] = 0x41;
  }
  string_contents[length-1] = 0;
  // Simple case.
  Local<String> string =
      String::NewExternalTwoByte(
          isolate, new TestResource(string_contents, nullptr, false))
          .ToLocalChecked();
  CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
  // Counter example.
  string = String::NewFromTwoByte(isolate, string_contents,
                                  v8::NewStringType::kNormal)
               .ToLocalChecked();
  CHECK(string->IsOneByte() && string->ContainsOnlyOneByte());
  // Test left right and balanced cons strings.
  Local<String> base = v8_str("a");
  Local<String> left = base;
  Local<String> right = base;
  for (int i = 0; i < 1000; i++) {
    left = String::Concat(isolate, base, left);
    right = String::Concat(isolate, right, base);
  }
  Local<String> balanced = String::Concat(isolate, left, base);
  balanced = String::Concat(isolate, balanced, right);
  Local<String> cons_strings[] = {left, balanced, right};
  Local<String> two_byte =
      String::NewExternalTwoByte(
          isolate, new TestResource(string_contents, nullptr, false))
          .ToLocalChecked();
  USE(two_byte); USE(cons_strings);
  for (size_t i = 0; i < arraysize(cons_strings); i++) {
    // Base assumptions.
    string = cons_strings[i];
    CHECK(string->IsOneByte() && string->ContainsOnlyOneByte());
    // Test left and right concatentation.
    string = String::Concat(isolate, two_byte, cons_strings[i]);
    CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
    string = String::Concat(isolate, cons_strings[i], two_byte);
    CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
  }
  // Set bits in different positions
  // for strings of different lengths and alignments.
  for (int alignment = 0; alignment < 7; alignment++) {
    for (int size = 2; alignment + size < length; size *= 2) {
      int zero_offset = size + alignment;
      string_contents[zero_offset] = 0;
      for (int i = 0; i < size; i++) {
        int shift = 8 + (i % 7);
        string_contents[alignment + i] = 1 << shift;
        string = String::NewExternalTwoByte(
                     isolate, new TestResource(string_contents + alignment,
                                               nullptr, false))
                     .ToLocalChecked();
        CHECK_EQ(size, string->Length());
        CHECK(!string->ContainsOnlyOneByte());
        string_contents[alignment + i] = 0x41;
      }
      string_contents[zero_offset] = 0x41;
    }
  }
}


// Failed access check callback that performs a GC on each invocation.
void FailedAccessCheckCallbackGC(Local<v8::Object> target,
                                 v8::AccessType type,
                                 Local<v8::Value> data) {
  CcTest::CollectAllGarbage();
  CcTest::isolate()->ThrowException(
      v8::Exception::Error(v8_str("cross context")));
}


TEST(GCInFailedAccessCheckCallback) {
  // Install a failed access check callback that performs a GC on each
  // invocation. Then force the callback to be called from va

  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();

  isolate->SetFailedAccessCheckCallbackFunction(&FailedAccessCheckCallbackGC);

  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallback(AccessAlwaysBlocked);

  // Create a context and set an x property on it's global object.
  LocalContext context0(nullptr, global_template);
  CHECK(context0->Global()
            ->Set(context0.local(), v8_str("x"), v8_num(42))
            .FromJust());
  v8::Local<v8::Object> global0 = context0->Global();

  // Create a context with a different security token so that the
  // failed access check callback will be called on each access.
  LocalContext context1(nullptr, global_template);
  CHECK(context1->Global()
            ->Set(context1.local(), v8_str("other"), global0)
            .FromJust());

  v8::TryCatch try_catch(isolate);

  // Get property with failed access check.
  CHECK(CompileRun("other.x").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Get element with failed access check.
  CHECK(CompileRun("other[0]").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Set property with failed access check.
  CHECK(CompileRun("other.x = new Object()").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Set element with failed access check.
  CHECK(CompileRun("other[0] = new Object()").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Get property attribute with failed access check.
  CHECK(CompileRun("\'x\' in other").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Get property attribute for element with failed access check.
  CHECK(CompileRun("0 in other").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Delete property.
  CHECK(CompileRun("delete other.x").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Delete element.
  CHECK(global0->Delete(context1.local(), 0).IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // DefineAccessor.
  CHECK(global0
            ->SetAccessor(context1.local(), v8_str("x"), GetXValue, nullptr,
                          v8_str("x"))
            .IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Define JavaScript accessor.
  CHECK(CompileRun(
            "Object.prototype.__defineGetter__.call("
            "    other, \'x\', function() { return 42; })").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // LookupAccessor.
  CHECK(CompileRun(
            "Object.prototype.__lookupGetter__.call("
            "    other, \'x\')").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // HasOwnElement.
  CHECK(CompileRun(
            "Object.prototype.hasOwnProperty.call("
            "other, \'0\')").IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(global0->HasRealIndexedProperty(context1.local(), 0).IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(
      global0->HasRealNamedProperty(context1.local(), v8_str("x")).IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  CHECK(global0->HasRealNamedCallbackProperty(context1.local(), v8_str("x"))
            .IsNothing());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Reset the failed access check callback so it does not influence
  // the other tests.
  isolate->SetFailedAccessCheckCallbackFunction(nullptr);
}


TEST(IsolateNewDispose) {
  v8::Isolate* current_isolate = CcTest::isolate();
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  CHECK_NOT_NULL(isolate);
  CHECK(current_isolate != isolate);
  CHECK(current_isolate == CcTest::isolate());
  CHECK(isolate->GetArrayBufferAllocator() == CcTest::array_buffer_allocator());

  isolate->SetFatalErrorHandler(StoringErrorCallback);
  last_location = last_message = nullptr;
  isolate->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);
}


UNINITIALIZED_TEST(DisposeIsolateWhenInUse) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope scope(isolate);
    LocalContext context(isolate);
    // Run something in this isolate.
    ExpectTrue("true");
    isolate->SetFatalErrorHandler(StoringErrorCallback);
    last_location = last_message = nullptr;
    // Still entered, should fail.
    isolate->Dispose();
    CHECK(last_location);
    CHECK(last_message);
  }
  isolate->Dispose();
}


static void BreakArrayGuarantees(const char* script) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate1 = v8::Isolate::New(create_params);
  isolate1->Enter();
  v8::Persistent<v8::Context> context1;
  {
    v8::HandleScope scope(isolate1);
    context1.Reset(isolate1, Context::New(isolate1));
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    v8::internal::Isolate* i_isolate =
        reinterpret_cast<v8::internal::Isolate*>(isolate1);
    CHECK(i_isolate->IsNoElementsProtectorIntact());
    // Run something in new isolate.
    CompileRun(script);
    CHECK(!i_isolate->IsNoElementsProtectorIntact());
  }
  isolate1->Exit();
  isolate1->Dispose();
}


TEST(VerifyArrayPrototypeGuarantees) {
  // Break fast array hole handling by element changes.
  BreakArrayGuarantees("[].__proto__[1] = 3;");
  BreakArrayGuarantees("Object.prototype[3] = 'three';");
  BreakArrayGuarantees("Array.prototype.push(1);");
  BreakArrayGuarantees("Array.prototype.unshift(1);");
  // Break fast array hole handling by changing length.
  BreakArrayGuarantees("Array.prototype.length = 30;");
  // Break fast array hole handling by prototype structure changes.
  BreakArrayGuarantees("[].__proto__.__proto__ = { funny: true };");
  // By sending elements to dictionary mode.
  BreakArrayGuarantees(
      "Object.defineProperty(Array.prototype, 0, {"
      "  get: function() { return 3; }});");
  BreakArrayGuarantees(
      "Object.defineProperty(Object.prototype, 0, {"
      "  get: function() { return 3; }});");
}


TEST(RunTwoIsolatesOnSingleThread) {
  // Run isolate 1.
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate1 = v8::Isolate::New(create_params);
  isolate1->Enter();
  v8::Persistent<v8::Context> context1;
  {
    v8::HandleScope scope(isolate1);
    context1.Reset(isolate1, Context::New(isolate1));
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    // Run something in new isolate.
    CompileRun("var foo = 'isolate 1';");
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  // Run isolate 2.
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  v8::Persistent<v8::Context> context2;

  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    context2.Reset(isolate2, Context::New(isolate2));
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate2, context2);
    v8::Context::Scope context_scope(context);

    // Run something in new isolate.
    CompileRun("var foo = 'isolate 2';");
    ExpectString("function f() { return foo; }; f()", "isolate 2");
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    // Now again in isolate 1
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  isolate1->Exit();

  // Run some stuff in default isolate.
  v8::Persistent<v8::Context> context_default;
  {
    v8::Isolate* isolate = CcTest::isolate();
    v8::Isolate::Scope iscope(isolate);
    v8::HandleScope scope(isolate);
    context_default.Reset(isolate, Context::New(isolate));
  }

  {
    v8::HandleScope scope(CcTest::isolate());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(CcTest::isolate(), context_default);
    v8::Context::Scope context_scope(context);
    // Variables in other isolates should be not available, verify there
    // is an exception.
    ExpectTrue("function f() {"
               "  try {"
               "    foo;"
               "    return false;"
               "  } catch(e) {"
               "    return true;"
               "  }"
               "};"
               "var isDefaultIsolate = true;"
               "f()");
  }

  isolate1->Enter();

  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate2, context2);
    v8::Context::Scope context_scope(context);
    ExpectString("function f() { return foo; }; f()", "isolate 2");
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  {
    v8::Isolate::Scope iscope(isolate2);
    context2.Reset();
  }

  context1.Reset();
  isolate1->Exit();

  isolate2->SetFatalErrorHandler(StoringErrorCallback);
  last_location = last_message = nullptr;

  isolate1->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);

  isolate2->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);

  // Check that default isolate still runs.
  {
    v8::HandleScope scope(CcTest::isolate());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(CcTest::isolate(), context_default);
    v8::Context::Scope context_scope(context);
    ExpectTrue("function f() { return isDefaultIsolate; }; f()");
  }
}


static int CalcFibonacci(v8::Isolate* isolate, int limit) {
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope scope(isolate);
  LocalContext context(isolate);
  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, "function fib(n) {"
                    "  if (n <= 2) return 1;"
                    "  return fib(n-1) + fib(n-2);"
                    "}"
                    "fib(%d)", limit);
  Local<Value> value = CompileRun(code.begin());
  CHECK(value->IsNumber());
  return static_cast<int>(value->NumberValue(context.local()).FromJust());
}

class IsolateThread : public v8::base::Thread {
 public:
  explicit IsolateThread(int fib_limit)
      : Thread(Options("IsolateThread")), fib_limit_(fib_limit), result_(0) {}

  void Run() override {
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    result_ = CalcFibonacci(isolate, fib_limit_);
    isolate->Dispose();
  }

  int result() { return result_; }

 private:
  int fib_limit_;
  int result_;
};


TEST(MultipleIsolatesOnIndividualThreads) {
  IsolateThread thread1(21);
  IsolateThread thread2(12);

  // Compute some fibonacci numbers on 3 threads in 3 isolates.
  CHECK(thread1.Start());
  CHECK(thread2.Start());

  int result1 = CalcFibonacci(CcTest::isolate(), 21);
  int result2 = CalcFibonacci(CcTest::isolate(), 12);

  thread1.Join();
  thread2.Join();

  // Compare results. The actual fibonacci numbers for 12 and 21 are taken
  // (I'm lazy!) from http://en.wikipedia.org/wiki/Fibonacci_number
  CHECK_EQ(result1, 10946);
  CHECK_EQ(result2, 144);
  CHECK_EQ(result1, thread1.result());
  CHECK_EQ(result2, thread2.result());
}


TEST(IsolateDifferentContexts) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  Local<v8::Context> context;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    Local<Value> v = CompileRun("2");
    CHECK(v->IsNumber());
    CHECK_EQ(2, static_cast<int>(v->NumberValue(context).FromJust()));
  }
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    Local<Value> v = CompileRun("22");
    CHECK(v->IsNumber());
    CHECK_EQ(22, static_cast<int>(v->NumberValue(context).FromJust()));
  }
  isolate->Dispose();
}

class InitDefaultIsolateThread : public v8::base::Thread {
 public:
  enum TestCase {
    SetFatalHandler,
    SetCounterFunction,
    SetCreateHistogramFunction,
    SetAddHistogramSampleFunction
  };

  explicit InitDefaultIsolateThread(TestCase testCase)
      : Thread(Options("InitDefaultIsolateThread")),
        testCase_(testCase),
        result_(false) {}

  void Run() override {
    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    isolate->Enter();
    switch (testCase_) {
      case SetFatalHandler:
        isolate->SetFatalErrorHandler(nullptr);
        break;

      case SetCounterFunction:
        CcTest::isolate()->SetCounterFunction(nullptr);
        break;

      case SetCreateHistogramFunction:
        CcTest::isolate()->SetCreateHistogramFunction(nullptr);
        break;

      case SetAddHistogramSampleFunction:
        CcTest::isolate()->SetAddHistogramSampleFunction(nullptr);
        break;
    }
    isolate->Exit();
    isolate->Dispose();
    result_ = true;
  }

  bool result() { return result_; }

 private:
  TestCase testCase_;
  bool result_;
};


static void InitializeTestHelper(InitDefaultIsolateThread::TestCase testCase) {
  InitDefaultIsolateThread thread(testCase);
  CHECK(thread.Start());
  thread.Join();
  CHECK(thread.result());
}

TEST(InitializeDefaultIsolateOnSecondaryThread_FatalHandler) {
  InitializeTestHelper(InitDefaultIsolateThread::SetFatalHandler);
}

TEST(InitializeDefaultIsolateOnSecondaryThread_CounterFunction) {
  InitializeTestHelper(InitDefaultIsolateThread::SetCounterFunction);
}

TEST(InitializeDefaultIsolateOnSecondaryThread_CreateHistogramFunction) {
  InitializeTestHelper(InitDefaultIsolateThread::SetCreateHistogramFunction);
}

TEST(InitializeDefaultIsolateOnSecondaryThread_AddHistogramSampleFunction) {
  InitializeTestHelper(InitDefaultIsolateThread::SetAddHistogramSampleFunction);
}


TEST(StringCheckMultipleContexts) {
  const char* code =
      "(function() { return \"a\".charAt(0); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "a");
    ExpectString(code, "a");
  }

  {
    // Change the String.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("String.prototype.charAt = function() { return \"not a\"; }");
    ExpectString(code, "not a");
  }
}


TEST(NumberCheckMultipleContexts) {
  const char* code =
      "(function() { return (42).toString(); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "42");
    ExpectString(code, "42");
  }

  {
    // Change the Number.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("Number.prototype.toString = function() { return \"not 42\"; }");
    ExpectString(code, "not 42");
  }
}


TEST(BooleanCheckMultipleContexts) {
  const char* code =
      "(function() { return true.toString(); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "true");
    ExpectString(code, "true");
  }

  {
    // Change the Boolean.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("Boolean.prototype.toString = function() { return \"\"; }");
    ExpectString(code, "");
  }
}


TEST(DontDeleteCellLoadIC) {
  const char* function_code =
      "function readCell() { while (true) { return cell; } }";

  {
    // Run the code twice in the first context to initialize the load
    // IC for a don't delete cell.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    CompileRun("var cell = \"first\";");
    ExpectBoolean("delete cell", false);
    CompileRun(function_code);
    ExpectString("readCell()", "first");
    ExpectString("readCell()", "first");
  }

  {
    // Use a deletable cell in the second context.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("cell = \"second\";");
    CompileRun(function_code);
    ExpectString("readCell()", "second");
    ExpectBoolean("delete cell", true);
    ExpectString("(function() {"
                 "  try {"
                 "    return readCell();"
                 "  } catch(e) {"
                 "    return e.toString();"
                 "  }"
                 "})()",
                 "ReferenceError: cell is not defined");
    CompileRun("cell = \"new_second\";");
    CcTest::CollectAllGarbage();
    ExpectString("readCell()", "new_second");
    ExpectString("readCell()", "new_second");
  }
}


class Visitor42 : public v8::PersistentHandleVisitor {
 public:
  explicit Visitor42(v8::Persistent<v8::Object>* object)
      : counter_(0), object_(object) { }

  void VisitPersistentHandle(Persistent<Value>* value,
                             uint16_t class_id) override {
    if (class_id != 42) return;
    CHECK_EQ(42, value->WrapperClassId());
    v8::Isolate* isolate = CcTest::isolate();
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Value> handle = v8::Local<v8::Value>::New(isolate, *value);
    v8::Local<v8::Value> object = v8::Local<v8::Object>::New(isolate, *object_);
    CHECK(handle->IsObject());
    CHECK(Local<Object>::Cast(handle)
              ->Equals(isolate->GetCurrentContext(), object)
              .FromJust());
    ++counter_;
  }

  int counter_;
  v8::Persistent<v8::Object>* object_;
};


TEST(PersistentHandleVisitor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Persistent<v8::Object> object(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object.WrapperClassId());
  object.SetWrapperClassId(42);
  CHECK_EQ(42, object.WrapperClassId());

  Visitor42 visitor(&object);
  isolate->VisitHandlesWithClassIds(&visitor);
  CHECK_EQ(1, visitor.counter_);

  object.Reset();
}


TEST(WrapperClassId) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Persistent<v8::Object> object(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object.WrapperClassId());
  object.SetWrapperClassId(65535);
  CHECK_EQ(65535, object.WrapperClassId());
  object.Reset();
}

TEST(RegExp) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  v8::Local<v8::RegExp> re =
      v8::RegExp::New(context.local(), v8_str("foo"), v8::RegExp::kNone)
          .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("foo")).FromJust());
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = v8::RegExp::New(context.local(), v8_str("bar"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kGlobal))
           .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("bar")).FromJust());
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kGlobal,
           static_cast<int>(re->GetFlags()));

  re = v8::RegExp::New(context.local(), v8_str("baz"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kMultiline))
           .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("baz")).FromJust());
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  re = v8::RegExp::New(context.local(), v8_str("baz"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kUnicode |
                                                      v8::RegExp::kSticky))
           .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("baz")).FromJust());
  CHECK_EQ(v8::RegExp::kUnicode | v8::RegExp::kSticky,
           static_cast<int>(re->GetFlags()));

  re = CompileRun("/quux/").As<v8::RegExp>();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("quux")).FromJust());
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = CompileRun("/quux/gm").As<v8::RegExp>();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("quux")).FromJust());
  CHECK_EQ(v8::RegExp::kGlobal | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  // Override the RegExp constructor and check the API constructor
  // still works.
  CompileRun("RegExp = function() {}");

  re = v8::RegExp::New(context.local(), v8_str("foobar"), v8::RegExp::kNone)
           .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(context.local(), v8_str("foobar")).FromJust());
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = v8::RegExp::New(context.local(), v8_str("foobarbaz"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kMultiline))
           .ToLocalChecked();
  CHECK(re->IsRegExp());
  CHECK(
      re->GetSource()->Equals(context.local(), v8_str("foobarbaz")).FromJust());
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  CHECK(context->Global()->Set(context.local(), v8_str("re"), re).FromJust());
  ExpectTrue("re.test('FoobarbaZ')");

  // RegExps are objects on which you can set properties.
  re->Set(context.local(), v8_str("property"),
          v8::Integer::New(context->GetIsolate(), 32))
      .FromJust();
  v8::Local<v8::Value> value(CompileRun("re.property"));
  CHECK_EQ(32, value->Int32Value(context.local()).FromJust());

  v8::TryCatch try_catch(context->GetIsolate());
  CHECK(v8::RegExp::New(context.local(), v8_str("foo["), v8::RegExp::kNone)
            .IsEmpty());
  CHECK(try_catch.HasCaught());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("ex"), try_catch.Exception())
            .FromJust());
  ExpectTrue("ex instanceof SyntaxError");
}


THREADED_TEST(Equals) {
  LocalContext localContext;
  v8::HandleScope handleScope(localContext->GetIsolate());

  v8::Local<v8::Object> globalProxy = localContext->Global();
  v8::Local<Value> global = globalProxy->GetPrototype();

  CHECK(global->StrictEquals(global));
  CHECK(!global->StrictEquals(globalProxy));
  CHECK(!globalProxy->StrictEquals(global));
  CHECK(globalProxy->StrictEquals(globalProxy));

  CHECK(global->Equals(localContext.local(), global).FromJust());
  CHECK(!global->Equals(localContext.local(), globalProxy).FromJust());
  CHECK(!globalProxy->Equals(localContext.local(), global).FromJust());
  CHECK(globalProxy->Equals(localContext.local(), globalProxy).FromJust());
}


static void Getter(v8::Local<v8::Name> property,
                   const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_str("42!"));
}


static void Enumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
  v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate());
  result->Set(info.GetIsolate()->GetCurrentContext(), 0,
              v8_str("universalAnswer"))
      .FromJust();
  info.GetReturnValue().Set(result);
}


TEST(NamedEnumeratorAndForIn) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context.local());

  v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetHandler(v8::NamedPropertyHandlerConfiguration(
      Getter, nullptr, nullptr, nullptr, Enumerator));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("o"),
                  tmpl->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  v8::Local<v8::Array> result = v8::Local<v8::Array>::Cast(
      CompileRun("var result = []; for (var k in o) result.push(k); result"));
  CHECK_EQ(1u, result->Length());
  CHECK(v8_str("universalAnswer")
            ->Equals(context.local(),
                     result->Get(context.local(), 0).ToLocalChecked())
            .FromJust());
}


TEST(DefinePropertyPostDetach) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<v8::Object> proxy = context->Global();
  v8::Local<v8::Function> define_property =
      CompileRun(
          "(function() {"
          "  Object.defineProperty("
          "    this,"
          "    1,"
          "    { configurable: true, enumerable: true, value: 3 });"
          "})")
          .As<Function>();
  context->DetachGlobal();
  CHECK(define_property->Call(context.local(), proxy, 0, nullptr).IsEmpty());
}


static void InstallContextId(v8::Local<Context> context, int id) {
  Context::Scope scope(context);
  CHECK(CompileRun("Object.prototype")
            .As<Object>()
            ->Set(context, v8_str("context_id"),
                  v8::Integer::New(context->GetIsolate(), id))
            .FromJust());
}


static void CheckContextId(v8::Local<Object> object, int expected) {
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  CHECK_EQ(expected, object->Get(context, v8_str("context_id"))
                         .ToLocalChecked()
                         ->Int32Value(context)
                         .FromJust());
}


THREADED_TEST(CreationContext) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  Local<Context> context1 = Context::New(isolate);
  InstallContextId(context1, 1);
  Local<Context> context2 = Context::New(isolate);
  InstallContextId(context2, 2);
  Local<Context> context3 = Context::New(isolate);
  InstallContextId(context3, 3);

  Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);

  Local<Object> object1;
  Local<Function> func1;
  {
    Context::Scope scope(context1);
    object1 = Object::New(isolate);
    func1 = tmpl->GetFunction(context1).ToLocalChecked();
  }

  Local<Object> object2;
  Local<Function> func2;
  {
    Context::Scope scope(context2);
    object2 = Object::New(isolate);
    func2 = tmpl->GetFunction(context2).ToLocalChecked();
  }

  Local<Object> instance1;
  Local<Object> instance2;

  {
    Context::Scope scope(context3);
    instance1 = func1->NewInstance(context3).ToLocalChecked();
    instance2 = func2->NewInstance(context3).ToLocalChecked();
  }

  {
    Local<Context> other_context = Context::New(isolate);
    Context::Scope scope(other_context);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }

  {
    Context::Scope scope(context1);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }

  {
    Context::Scope scope(context2);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }
}


THREADED_TEST(CreationContextOfJsFunction) {
  HandleScope handle_scope(CcTest::isolate());
  Local<Context> context = Context::New(CcTest::isolate());
  InstallContextId(context, 1);

  Local<Object> function;
  {
    Context::Scope scope(context);
    function = CompileRun("function foo() {}; foo").As<Object>();
  }

  Local<Context> other_context = Context::New(CcTest::isolate());
  Context::Scope scope(other_context);
  CHECK(function->CreationContext() == context);
  CheckContextId(function, 1);
}


THREADED_TEST(CreationContextOfJsBoundFunction) {
  HandleScope handle_scope(CcTest::isolate());
  Local<Context> context1 = Context::New(CcTest::isolate());
  InstallContextId(context1, 1);
  Local<Context> context2 = Context::New(CcTest::isolate());
  InstallContextId(context2, 2);

  Local<Function> target_function;
  {
    Context::Scope scope(context1);
    target_function = CompileRun("function foo() {}; foo").As<Function>();
  }

  Local<Function> bound_function1, bound_function2;
  {
    Context::Scope scope(context2);
    CHECK(context2->Global()
              ->Set(context2, v8_str("foo"), target_function)
              .FromJust());
    bound_function1 = CompileRun("foo.bind(1)").As<Function>();
    bound_function2 =
        CompileRun("Function.prototype.bind.call(foo, 2)").As<Function>();
  }

  Local<Context> other_context = Context::New(CcTest::isolate());
  Context::Scope scope(other_context);
  CHECK(bound_function1->CreationContext() == context1);
  CheckContextId(bound_function1, 1);
  CHECK(bound_function2->CreationContext() == context1);
  CheckContextId(bound_function2, 1);
}


void HasOwnPropertyIndexedPropertyGetter(
    uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (index == 42) info.GetReturnValue().Set(v8_str("yes"));
}


void HasOwnPropertyNamedPropertyGetter(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (property->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
          .FromJust()) {
    info.GetReturnValue().Set(v8_str("yes"));
  }
}


void HasOwnPropertyIndexedPropertyQuery(
    uint32_t index, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (index == 42) info.GetReturnValue().Set(1);
}


void HasOwnPropertyNamedPropertyQuery(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (property->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
          .FromJust()) {
    info.GetReturnValue().Set(1);
  }
}


void HasOwnPropertyNamedPropertyQuery2(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (property->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("bar"))
          .FromJust()) {
    info.GetReturnValue().Set(1);
  }
}

void HasOwnPropertyAccessorGetter(
    Local<String> property,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_str("yes"));
}

void HasOwnPropertyAccessorNameGetter(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_str("yes"));
}

TEST(HasOwnProperty) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  { // Check normal properties and defined getters.
    Local<Value> value = CompileRun(
        "function Foo() {"
        "    this.foo = 11;"
        "    this.__defineGetter__('baz', function() { return 1; });"
        "};"
        "function Bar() { "
        "    this.bar = 13;"
        "    this.__defineGetter__('bla', function() { return 2; });"
        "};"
        "Bar.prototype = new Foo();"
        "new Bar();");
    CHECK(value->IsObject());
    Local<Object> object = value->ToObject(env.local()).ToLocalChecked();
    CHECK(object->Has(env.local(), v8_str("foo")).FromJust());
    CHECK(!object->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
    CHECK(object->HasOwnProperty(env.local(), v8_str("bar")).FromJust());
    CHECK(object->Has(env.local(), v8_str("baz")).FromJust());
    CHECK(!object->HasOwnProperty(env.local(), v8_str("baz")).FromJust());
    CHECK(object->HasOwnProperty(env.local(), v8_str("bla")).FromJust());
  }
  { // Check named getter interceptors.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        HasOwnPropertyNamedPropertyGetter));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("42")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), 42).FromJust());
    CHECK(instance->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("bar")).FromJust());
  }
  { // Check indexed getter interceptors.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        HasOwnPropertyIndexedPropertyGetter));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(instance->HasOwnProperty(env.local(), v8_str("42")).FromJust());
    CHECK(instance->HasOwnProperty(env.local(), 42).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("43")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), 43).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
  }
  { // Check named query interceptors.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        nullptr, nullptr, HasOwnPropertyNamedPropertyQuery));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(instance->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("bar")).FromJust());
  }
  { // Check indexed query interceptors.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        nullptr, nullptr, HasOwnPropertyIndexedPropertyQuery));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(instance->HasOwnProperty(env.local(), v8_str("42")).FromJust());
    CHECK(instance->HasOwnProperty(env.local(), 42).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("41")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), 41).FromJust());
  }
  { // Check callbacks.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetAccessor(v8_str("foo"), HasOwnPropertyAccessorGetter);
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(instance->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("bar")).FromJust());
  }
  { // Check that query wins on disagreement.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        HasOwnPropertyNamedPropertyGetter, nullptr,
        HasOwnPropertyNamedPropertyQuery2));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    CHECK(!instance->HasOwnProperty(env.local(), v8_str("foo")).FromJust());
    CHECK(instance->HasOwnProperty(env.local(), v8_str("bar")).FromJust());
  }
  {  // Check that non-internalized keys are handled correctly.
    Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        HasOwnPropertyAccessorNameGetter));
    Local<Object> instance = templ->NewInstance(env.local()).ToLocalChecked();
    env->Global()->Set(env.local(), v8_str("obj"), instance).FromJust();
    const char* src =
        "var dyn_string = 'this string ';"
        "dyn_string += 'does not exist elsewhere';"
        "({}).hasOwnProperty.call(obj, dyn_string)";
    CHECK(CompileRun(src)->BooleanValue(isolate));
  }
}


TEST(IndexedInterceptorWithStringProto) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
      nullptr, nullptr, HasOwnPropertyIndexedPropertyQuery));
  LocalContext context;
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun("var s = new String('foobar'); obj.__proto__ = s;");
  // These should be intercepted.
  CHECK(CompileRun("42 in obj")->BooleanValue(isolate));
  CHECK(CompileRun("'42' in obj")->BooleanValue(isolate));
  // These should fall through to the String prototype.
  CHECK(CompileRun("0 in obj")->BooleanValue(isolate));
  CHECK(CompileRun("'0' in obj")->BooleanValue(isolate));
  // And these should both fail.
  CHECK(!CompileRun("32 in obj")->BooleanValue(isolate));
  CHECK(!CompileRun("'32' in obj")->BooleanValue(isolate));
}


void CheckCodeGenerationAllowed() {
  Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  Local<Value> result = CompileRun("eval('42')");
  CHECK_EQ(42, result->Int32Value(context).FromJust());
  result = CompileRun("(function(e) { return e('42'); })(eval)");
  CHECK_EQ(42, result->Int32Value(context).FromJust());
  result = CompileRun("var f = new Function('return 42'); f()");
  CHECK_EQ(42, result->Int32Value(context).FromJust());
}


void CheckCodeGenerationDisallowed() {
  TryCatch try_catch(CcTest::isolate());

  Local<Value> result = CompileRun("eval('42')");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  result = CompileRun("(function(e) { return e('42'); })(eval)");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  result = CompileRun("var f = new Function('return 42'); f()");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
}

char first_fourty_bytes[41];

bool CodeGenerationAllowed(Local<Context> context, Local<String> source) {
  String::Utf8Value str(CcTest::isolate(), source);
  size_t len = std::min(sizeof(first_fourty_bytes) - 1,
                        static_cast<size_t>(str.length()));
  strncpy(first_fourty_bytes, *str, len);
  first_fourty_bytes[len] = 0;
  ApiTestFuzzer::Fuzz();
  return true;
}

bool CodeGenerationDisallowed(Local<Context> context, Local<String> source) {
  ApiTestFuzzer::Fuzz();
  return false;
}

v8::MaybeLocal<String> ModifyCodeGeneration(Local<Context> context,
                                            Local<Value> source) {
  // For testing purposes, deny all odd-length strings and replace '2' with '3'
  String::Utf8Value utf8(context->GetIsolate(), source);
  DCHECK(utf8.length());
  if (utf8.length() == 0 || utf8.length() % 2 != 0)
    return v8::MaybeLocal<String>();

  for (char* i = *utf8; *i != '\0'; i++) {
    if (*i == '2') *i = '3';
  }
  return String::NewFromUtf8(context->GetIsolate(), *utf8,
                             v8::NewStringType::kNormal)
      .ToLocalChecked();
}

THREADED_TEST(AllowCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // eval and the Function constructor allowed by default.
  CHECK(context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationAllowed();

  // Disallow eval and the Function constructor.
  context->AllowCodeGenerationFromStrings(false);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationDisallowed();

  // Allow again.
  context->AllowCodeGenerationFromStrings(true);
  CheckCodeGenerationAllowed();

  // Disallow but setting a global callback that will allow the calls.
  context->AllowCodeGenerationFromStrings(false);
  context->GetIsolate()->SetAllowCodeGenerationFromStringsCallback(
      &CodeGenerationAllowed);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationAllowed();

  // Set a callback that disallows the code generation.
  context->GetIsolate()->SetAllowCodeGenerationFromStringsCallback(
      &CodeGenerationDisallowed);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationDisallowed();
}

TEST(ModifyCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  context->AllowCodeGenerationFromStrings(false);
  context->GetIsolate()->SetModifyCodeGenerationFromStringsCallback(
      &ModifyCodeGeneration);

  // Test 'allowed' case in different modes (direct eval, indirect eval,
  // Function constructor, Function contructor with arguments).
  Local<Value> result = CompileRun("eval('42')");
  CHECK_EQ(43, result->Int32Value(context.local()).FromJust());

  result = CompileRun("(function(e) { return e('42'); })(eval)");
  CHECK_EQ(43, result->Int32Value(context.local()).FromJust());

  result = CompileRun("var f = new Function('return 42;'); f()");
  CHECK_EQ(43, result->Int32Value(context.local()).FromJust());

  // Test 'disallowed' cases.
  TryCatch try_catch(CcTest::isolate());
  result = CompileRun("eval('123')");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  result = CompileRun("new Function('a', 'return 42;')(123)");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
}

TEST(SetErrorMessageForCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  TryCatch try_catch(context->GetIsolate());

  Local<String> message = v8_str("Message");
  Local<String> expected_message = v8_str("Uncaught EvalError: Message");
  context->GetIsolate()->SetAllowCodeGenerationFromStringsCallback(
      &CodeGenerationDisallowed);
  context->AllowCodeGenerationFromStrings(false);
  context->SetErrorMessageForCodeGenerationFromStrings(message);
  Local<Value> result = CompileRun("eval('42')");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  Local<String> actual_message = try_catch.Message()->Get();
  CHECK(expected_message->Equals(context.local(), actual_message).FromJust());
}

TEST(CaptureSourceForCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  TryCatch try_catch(context->GetIsolate());

  context->GetIsolate()->SetAllowCodeGenerationFromStringsCallback(
      &CodeGenerationAllowed);
  context->AllowCodeGenerationFromStrings(false);
  CompileRun("eval('42')");
  CHECK(!strcmp(first_fourty_bytes, "42"));
}

static void NonObjectThis(const v8::FunctionCallbackInfo<v8::Value>& args) {
}


THREADED_TEST(CallAPIFunctionOnNonObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<FunctionTemplate> templ =
      v8::FunctionTemplate::New(isolate, NonObjectThis);
  Local<Function> function =
      templ->GetFunction(context.local()).ToLocalChecked();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"), function)
            .FromJust());
  TryCatch try_catch(isolate);
  CompileRun("f.call(2)");
}


// Regression test for issue 1470.
THREADED_TEST(ReadOnlyIndexedProperties) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);

  LocalContext context;
  Local<v8::Object> obj = templ->NewInstance(context.local()).ToLocalChecked();
  CHECK(context->Global()->Set(context.local(), v8_str("obj"), obj).FromJust());
  obj->DefineOwnProperty(context.local(), v8_str("1"), v8_str("DONT_CHANGE"),
                         v8::ReadOnly)
      .FromJust();
  obj->Set(context.local(), v8_str("1"), v8_str("foobar")).FromJust();
  CHECK(v8_str("DONT_CHANGE")
            ->Equals(context.local(),
                     obj->Get(context.local(), v8_str("1")).ToLocalChecked())
            .FromJust());
  obj->DefineOwnProperty(context.local(), v8_str("2"), v8_str("DONT_CHANGE"),
                         v8::ReadOnly)
      .FromJust();
  obj->Set(context.local(), v8_num(2), v8_str("foobar")).FromJust();
  CHECK(v8_str("DONT_CHANGE")
            ->Equals(context.local(),
                     obj->Get(context.local(), v8_num(2)).ToLocalChecked())
            .FromJust());

  // Test non-smi case.
  obj->DefineOwnProperty(context.local(), v8_str("2000000000"),
                         v8_str("DONT_CHANGE"), v8::ReadOnly)
      .FromJust();
  obj->Set(context.local(), v8_str("2000000000"), v8_str("foobar")).FromJust();
  CHECK(v8_str("DONT_CHANGE")
            ->Equals(context.local(),
                     obj->Get(context.local(), v8_str("2000000000"))
                         .ToLocalChecked())
            .FromJust());
}

static int CountLiveMapsInMapCache(i::Context context) {
  i::WeakFixedArray map_cache = i::WeakFixedArray::cast(context.map_cache());
  int length = map_cache.length();
  int count = 0;
  for (int i = 0; i < length; i++) {
    if (map_cache.Get(i)->IsWeak()) count++;
  }
  return count;
}


THREADED_TEST(Regress1516) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Object with 20 properties is not a common case, so it should be removed
  // from the cache after GC.
  { v8::HandleScope temp_scope(context->GetIsolate());
    CompileRun(
        "({"
        "'a00': 0, 'a01': 0, 'a02': 0, 'a03': 0, 'a04': 0, "
        "'a05': 0, 'a06': 0, 'a07': 0, 'a08': 0, 'a09': 0, "
        "'a10': 0, 'a11': 0, 'a12': 0, 'a13': 0, 'a14': 0, "
        "'a15': 0, 'a16': 0, 'a17': 0, 'a18': 0, 'a19': 0, "
        "})");
  }

  int elements = CountLiveMapsInMapCache(CcTest::i_isolate()->context());
  CHECK_LE(1, elements);

  // We have to abort incremental marking here to abandon black pages.
  CcTest::PreciseCollectAllGarbage();

  CHECK_GT(elements, CountLiveMapsInMapCache(CcTest::i_isolate()->context()));
}


static void TestReceiver(Local<Value> expected_result,
                         Local<Value> expected_receiver,
                         const char* code) {
  Local<Value> result = CompileRun(code);
  Local<Context> context = CcTest::isolate()->GetCurrentContext();
  CHECK(result->IsObject());
  CHECK(expected_receiver
            ->Equals(context,
                     result.As<v8::Object>()->Get(context, 1).ToLocalChecked())
            .FromJust());
  CHECK(expected_result
            ->Equals(context,
                     result.As<v8::Object>()->Get(context, 0).ToLocalChecked())
            .FromJust());
}


THREADED_TEST(ForeignFunctionReceiver) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);

  // Create two contexts with different "id" properties ('i' and 'o').
  // Call a function both from its own context and from a the foreign
  // context, and see what "this" is bound to (returning both "this"
  // and "this.id" for comparison).

  Local<Context> foreign_context = v8::Context::New(isolate);
  foreign_context->Enter();
  Local<Value> foreign_function =
    CompileRun("function func() { return { 0: this.id, "
               "                           1: this, "
               "                           toString: function() { "
               "                               return this[0];"
               "                           }"
               "                         };"
               "}"
               "var id = 'i';"
               "func;");
  CHECK(foreign_function->IsFunction());
  foreign_context->Exit();

  LocalContext context;

  Local<String> password = v8_str("Password");
  // Don't get hit by security checks when accessing foreign_context's
  // global receiver (aka. global proxy).
  context->SetSecurityToken(password);
  foreign_context->SetSecurityToken(password);

  Local<String> i = v8_str("i");
  Local<String> o = v8_str("o");
  Local<String> id = v8_str("id");

  CompileRun("function ownfunc() { return { 0: this.id, "
             "                              1: this, "
             "                              toString: function() { "
             "                                  return this[0];"
             "                              }"
             "                             };"
             "}"
             "var id = 'o';"
             "ownfunc");
  CHECK(context->Global()
            ->Set(context.local(), v8_str("func"), foreign_function)
            .FromJust());

  // Sanity check the contexts.
  CHECK(
      i->Equals(
           context.local(),
           foreign_context->Global()->Get(context.local(), id).ToLocalChecked())
          .FromJust());
  CHECK(o->Equals(context.local(),
                  context->Global()->Get(context.local(), id).ToLocalChecked())
            .FromJust());

  // Checking local function's receiver.
  // Calling function using its call/apply methods.
  TestReceiver(o, context->Global(), "ownfunc.call()");
  TestReceiver(o, context->Global(), "ownfunc.apply()");
  // Making calls through built-in functions.
  TestReceiver(o, context->Global(), "[1].map(ownfunc)[0]");
  CHECK(
      o->Equals(context.local(), CompileRun("'abcbd'.replace(/b/,ownfunc)[1]"))
          .FromJust());
  CHECK(
      o->Equals(context.local(), CompileRun("'abcbd'.replace(/b/g,ownfunc)[1]"))
          .FromJust());
  CHECK(
      o->Equals(context.local(), CompileRun("'abcbd'.replace(/b/g,ownfunc)[3]"))
          .FromJust());
  // Calling with environment record as base.
  TestReceiver(o, context->Global(), "ownfunc()");
  // Calling with no base.
  TestReceiver(o, context->Global(), "(1,ownfunc)()");

  // Checking foreign function return value.
  // Calling function using its call/apply methods.
  TestReceiver(i, foreign_context->Global(), "func.call()");
  TestReceiver(i, foreign_context->Global(), "func.apply()");
  // Calling function using another context's call/apply methods.
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.call.call(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.call.apply(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.apply.call(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.apply.apply(func)");
  // Making calls through built-in functions.
  TestReceiver(i, foreign_context->Global(), "[1].map(func)[0]");
  // ToString(func()) is func()[0], i.e., the returned this.id.
  CHECK(i->Equals(context.local(), CompileRun("'abcbd'.replace(/b/,func)[1]"))
            .FromJust());
  CHECK(i->Equals(context.local(), CompileRun("'abcbd'.replace(/b/g,func)[1]"))
            .FromJust());
  CHECK(i->Equals(context.local(), CompileRun("'abcbd'.replace(/b/g,func)[3]"))
            .FromJust());

  // Calling with environment record as base.
  TestReceiver(i, foreign_context->Global(), "func()");
  // Calling with no base.
  TestReceiver(i, foreign_context->Global(), "(1,func)()");
}


uint8_t callback_fired = 0;
uint8_t before_call_entered_callback_count1 = 0;
uint8_t before_call_entered_callback_count2 = 0;


void CallCompletedCallback1(v8::Isolate*) {
  v8::base::OS::Print("Firing callback 1.\n");
  callback_fired ^= 1;  // Toggle first bit.
}


void CallCompletedCallback2(v8::Isolate*) {
  v8::base::OS::Print("Firing callback 2.\n");
  callback_fired ^= 2;  // Toggle second bit.
}


void BeforeCallEnteredCallback1(v8::Isolate*) {
  v8::base::OS::Print("Firing before call entered callback 1.\n");
  before_call_entered_callback_count1++;
}


void BeforeCallEnteredCallback2(v8::Isolate*) {
  v8::base::OS::Print("Firing before call entered callback 2.\n");
  before_call_entered_callback_count2++;
}


void RecursiveCall(const v8::FunctionCallbackInfo<v8::Value>& args) {
  int32_t level =
      args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromJust();
  if (level < 3) {
    level++;
    v8::base::OS::Print("Entering recursion level %d.\n", level);
    char script[64];
    i::Vector<char> script_vector(script, sizeof(script));
    i::SNPrintF(script_vector, "recursion(%d)", level);
    CompileRun(script_vector.begin());
    v8::base::OS::Print("Leaving recursion level %d.\n", level);
    CHECK_EQ(0, callback_fired);
  } else {
    v8::base::OS::Print("Recursion ends.\n");
    CHECK_EQ(0, callback_fired);
  }
}


TEST(CallCompletedCallback) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<v8::FunctionTemplate> recursive_runtime =
      v8::FunctionTemplate::New(env->GetIsolate(), RecursiveCall);
  env->Global()
      ->Set(env.local(), v8_str("recursion"),
            recursive_runtime->GetFunction(env.local()).ToLocalChecked())
      .FromJust();
  // Adding the same callback a second time has no effect.
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback1);
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback1);
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback2);
  env->GetIsolate()->AddBeforeCallEnteredCallback(BeforeCallEnteredCallback1);
  env->GetIsolate()->AddBeforeCallEnteredCallback(BeforeCallEnteredCallback2);
  env->GetIsolate()->AddBeforeCallEnteredCallback(BeforeCallEnteredCallback1);
  v8::base::OS::Print("--- Script (1) ---\n");
  callback_fired = 0;
  before_call_entered_callback_count1 = 0;
  before_call_entered_callback_count2 = 0;
  Local<Script> script =
      v8::Script::Compile(env.local(), v8_str("recursion(0)")).ToLocalChecked();
  script->Run(env.local()).ToLocalChecked();
  CHECK_EQ(3, callback_fired);
  CHECK_EQ(4, before_call_entered_callback_count1);
  CHECK_EQ(4, before_call_entered_callback_count2);

  v8::base::OS::Print("\n--- Script (2) ---\n");
  callback_fired = 0;
  before_call_entered_callback_count1 = 0;
  before_call_entered_callback_count2 = 0;
  env->GetIsolate()->RemoveCallCompletedCallback(CallCompletedCallback1);
  env->GetIsolate()->RemoveBeforeCallEnteredCallback(
      BeforeCallEnteredCallback1);
  script->Run(env.local()).ToLocalChecked();
  CHECK_EQ(2, callback_fired);
  CHECK_EQ(0, before_call_entered_callback_count1);
  CHECK_EQ(4, before_call_entered_callback_count2);

  v8::base::OS::Print("\n--- Function ---\n");
  callback_fired = 0;
  before_call_entered_callback_count1 = 0;
  before_call_entered_callback_count2 = 0;
  Local<Function> recursive_function = Local<Function>::Cast(
      env->Global()->Get(env.local(), v8_str("recursion")).ToLocalChecked());
  v8::Local<Value> args[] = {v8_num(0)};
  recursive_function->Call(env.local(), env->Global(), 1, args)
      .ToLocalChecked();
  CHECK_EQ(2, callback_fired);
  CHECK_EQ(0, before_call_entered_callback_count1);
  CHECK_EQ(4, before_call_entered_callback_count2);
}


void CallCompletedCallbackNoException(v8::Isolate*) {
  v8::HandleScope scope(CcTest::isolate());
  CompileRun("1+1;");
}


void CallCompletedCallbackException(v8::Isolate*) {
  v8::HandleScope scope(CcTest::isolate());
  CompileRun("throw 'second exception';");
}


TEST(CallCompletedCallbackOneException) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallbackNoException);
  CompileRun("throw 'exception';");
}


TEST(CallCompletedCallbackTwoExceptions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallbackException);
  CompileRun("throw 'first exception';");
}


static void MicrotaskOne(const v8::FunctionCallbackInfo<Value>& info) {
  CHECK(v8::MicrotasksScope::IsRunningMicrotasks(info.GetIsolate()));
  v8::HandleScope scope(info.GetIsolate());
  v8::MicrotasksScope microtasks(info.GetIsolate(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  CompileRun("ext1Calls++;");
}


static void MicrotaskTwo(const v8::FunctionCallbackInfo<Value>& info) {
  CHECK(v8::MicrotasksScope::IsRunningMicrotasks(info.GetIsolate()));
  v8::HandleScope scope(info.GetIsolate());
  v8::MicrotasksScope microtasks(info.GetIsolate(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
  CompileRun("ext2Calls++;");
}

void* g_passed_to_three = nullptr;

static void MicrotaskThree(void* data) {
  g_passed_to_three = data;
}


TEST(EnqueueMicrotask) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  CHECK(!v8::MicrotasksScope::IsRunningMicrotasks(env->GetIsolate()));
  CompileRun(
      "var ext1Calls = 0;"
      "var ext2Calls = 0;");
  CompileRun("1+1;");
  CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  g_passed_to_three = nullptr;
  env->GetIsolate()->EnqueueMicrotask(MicrotaskThree);
  CompileRun("1+1;");
  CHECK(!g_passed_to_three);
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());

  int dummy;
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  env->GetIsolate()->EnqueueMicrotask(MicrotaskThree, &dummy);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(&dummy, g_passed_to_three);
  CHECK_EQ(3, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  g_passed_to_three = nullptr;
}


static void MicrotaskExceptionOne(
    const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("exception1Calls++;");
  info.GetIsolate()->ThrowException(
      v8::Exception::Error(v8_str("first")));
}


static void MicrotaskExceptionTwo(
    const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("exception2Calls++;");
  info.GetIsolate()->ThrowException(
      v8::Exception::Error(v8_str("second")));
}


TEST(RunMicrotasksIgnoresThrownExceptions) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "var exception1Calls = 0;"
      "var exception2Calls = 0;");
  isolate->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskExceptionOne).ToLocalChecked());
  isolate->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskExceptionTwo).ToLocalChecked());
  TryCatch try_catch(isolate);
  CompileRun("1+1;");
  CHECK(!try_catch.HasCaught());
  CHECK_EQ(1,
           CompileRun("exception1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1,
           CompileRun("exception2Calls")->Int32Value(env.local()).FromJust());
}

static void ThrowExceptionMicrotask(void* data) {
  CcTest::isolate()->ThrowException(v8_str("exception"));
}

int microtask_callback_count = 0;

static void IncrementCounterMicrotask(void* data) {
  microtask_callback_count++;
}

TEST(RunMicrotasksIgnoresThrownExceptionsFromApi) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);
  {
    CHECK(!isolate->IsExecutionTerminating());
    isolate->EnqueueMicrotask(ThrowExceptionMicrotask);
    isolate->EnqueueMicrotask(IncrementCounterMicrotask);
    isolate->RunMicrotasks();
    CHECK_EQ(1, microtask_callback_count);
    CHECK(!try_catch.HasCaught());
  }
}

uint8_t microtasks_completed_callback_count = 0;

static void MicrotasksCompletedCallback(v8::Isolate* isolate, void*) {
  ++microtasks_completed_callback_count;
}

TEST(SetAutorunMicrotasks) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  env->GetIsolate()->AddMicrotasksCompletedCallback(
      &MicrotasksCompletedCallback);
  CompileRun(
      "var ext1Calls = 0;"
      "var ext2Calls = 0;");
  CompileRun("1+1;");
  CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0u, microtasks_completed_callback_count);

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1u, microtasks_completed_callback_count);

  env->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1u, microtasks_completed_callback_count);

  env->GetIsolate()->RunMicrotasks();
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2u, microtasks_completed_callback_count);

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2u, microtasks_completed_callback_count);

  env->GetIsolate()->RunMicrotasks();
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(3u, microtasks_completed_callback_count);

  env->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kAuto);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(4u, microtasks_completed_callback_count);

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  {
    v8::Isolate::SuppressMicrotaskExecutionScope scope(env->GetIsolate());
    CompileRun("1+1;");
    CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(4u, microtasks_completed_callback_count);
  }

  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(4, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(5u, microtasks_completed_callback_count);

  env->GetIsolate()->RemoveMicrotasksCompletedCallback(
      &MicrotasksCompletedCallback);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  CompileRun("1+1;");
  CHECK_EQ(3, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(4, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  CHECK_EQ(5u, microtasks_completed_callback_count);
}


TEST(RunMicrotasksWithoutEnteringContext) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  Local<Context> context = Context::New(isolate);
  {
    Context::Scope context_scope(context);
    CompileRun("var ext1Calls = 0;");
    isolate->EnqueueMicrotask(
        Function::New(context, MicrotaskOne).ToLocalChecked());
  }
  isolate->RunMicrotasks();
  {
    Context::Scope context_scope(context);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(context).FromJust());
  }
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kAuto);
}

static void Regress808911_MicrotaskCallback(void* data) {
  // So here we expect "current context" to be context1 and
  // "entered or microtask context" to be context2.
  v8::Isolate* isolate = static_cast<v8::Isolate*>(data);
  CHECK(isolate->GetCurrentContext() !=
        isolate->GetEnteredOrMicrotaskContext());
}

static void Regress808911_CurrentContextWrapper(
    const v8::FunctionCallbackInfo<Value>& info) {
  // So here we expect "current context" to be context1 and
  // "entered or microtask context" to be context2.
  v8::Isolate* isolate = info.GetIsolate();
  CHECK(isolate->GetCurrentContext() !=
        isolate->GetEnteredOrMicrotaskContext());
  isolate->EnqueueMicrotask(Regress808911_MicrotaskCallback, isolate);
  isolate->RunMicrotasks();
}

THREADED_TEST(Regress808911) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  Local<Context> context1 = Context::New(isolate);
  Local<Function> function;
  {
    Context::Scope context_scope(context1);
    function = Function::New(context1, Regress808911_CurrentContextWrapper)
                   .ToLocalChecked();
  }
  Local<Context> context2 = Context::New(isolate);
  Context::Scope context_scope(context2);
  function->CallAsFunction(context2, v8::Undefined(isolate), 0, nullptr)
      .ToLocalChecked();
}

TEST(ScopedMicrotasks) {
  LocalContext env;
  v8::HandleScope handles(env->GetIsolate());
  env->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kScoped);
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    CompileRun("var ext1Calls = 0;");
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    ExpectInt32("ext1Calls", 1);
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    CompileRun("throw new Error()");
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    ExpectInt32("ext1Calls", 2);
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    v8::TryCatch try_catch(env->GetIsolate());
    CompileRun("throw new Error()");
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    ExpectInt32("ext1Calls", 3);
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    env->GetIsolate()->TerminateExecution();
    {
      v8::MicrotasksScope scope2(env->GetIsolate(),
                                 v8::MicrotasksScope::kRunMicrotasks);
      env->GetIsolate()->EnqueueMicrotask(
          Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    }
  }
  env->GetIsolate()->CancelTerminateExecution();
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    ExpectInt32("ext1Calls", 3);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  }
  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);

    ExpectInt32("ext1Calls", 4);
  }

  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kDoNotRunMicrotasks);
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskOne).ToLocalChecked());
    CompileRun(
        "var ext1Calls = 0;"
        "var ext2Calls = 0;");
    CompileRun("1+1;");
    CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    {
      v8::MicrotasksScope scope2(env->GetIsolate(),
                                 v8::MicrotasksScope::kRunMicrotasks);
      CompileRun("1+1;");
      CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
      CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
      {
        v8::MicrotasksScope scope3(env->GetIsolate(),
                                   v8::MicrotasksScope::kRunMicrotasks);
        CompileRun("1+1;");
        CHECK_EQ(0,
                 CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
        CHECK_EQ(0,
                 CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
      }
      CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
      CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    }
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  }

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    CompileRun("1+1;");
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    {
      v8::MicrotasksScope scope2(env->GetIsolate(),
                                 v8::MicrotasksScope::kDoNotRunMicrotasks);
    }
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  }

  {
    v8::Isolate::SuppressMicrotaskExecutionScope scope1(env->GetIsolate());
    {
      v8::MicrotasksScope scope2(env->GetIsolate(),
                                 v8::MicrotasksScope::kRunMicrotasks);
    }
    v8::MicrotasksScope scope3(env->GetIsolate(),
                               v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  {
    v8::MicrotasksScope scope1(env->GetIsolate(),
                               v8::MicrotasksScope::kRunMicrotasks);
    v8::MicrotasksScope::PerformCheckpoint(env->GetIsolate());
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  v8::MicrotasksScope::PerformCheckpoint(env->GetIsolate());

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
    env->GetIsolate()->EnqueueMicrotask(
        Function::New(env.local(), MicrotaskTwo).ToLocalChecked());
  }

  v8::MicrotasksScope::PerformCheckpoint(env->GetIsolate());

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env.local(), MicrotaskOne).ToLocalChecked());
  {
    v8::Isolate::SuppressMicrotaskExecutionScope scope1(env->GetIsolate());
    v8::MicrotasksScope::PerformCheckpoint(env->GetIsolate());
    v8::MicrotasksScope scope2(env->GetIsolate(),
                               v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  v8::MicrotasksScope::PerformCheckpoint(env->GetIsolate());

  {
    v8::MicrotasksScope scope(env->GetIsolate(),
                              v8::MicrotasksScope::kDoNotRunMicrotasks);
    CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value(env.local()).FromJust());
    CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value(env.local()).FromJust());
  }

  env->GetIsolate()->SetMicrotasksPolicy(v8::MicrotasksPolicy::kAuto);
}

namespace {

void AssertCowElements(bool expected, const char* source) {
  Local<Value> object = CompileRun(source);
  i::Handle<i::JSObject> array =
      i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*object.As<Object>()));
  CHECK_EQ(expected, array->elements().IsCowArray());
}

}  // namespace

TEST(CheckCOWArraysCreatedRuntimeCounter) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  AssertCowElements(true, "[1, 2, 3]");
  AssertCowElements(false, "[[1], 2, 3]");
  AssertCowElements(true, "[[1], 2, 3][0]");
  AssertCowElements(true, "({foo: [4, 5, 6], bar: [3, 0]}.foo)");
  AssertCowElements(true, "({foo: [4, 5, 6], bar: [3, 0]}.bar)");
  AssertCowElements(false, "({foo: [1, 2, 3, [4, 5, 6]], bar: 'hi'}.foo)");
  AssertCowElements(true, "({foo: [1, 2, 3, [4, 5, 6]], bar: 'hi'}.foo[3])");
}


TEST(StaticGetters) {
  LocalContext context;
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  i::Handle<i::Object> undefined_value = factory->undefined_value();
  CHECK(*v8::Utils::OpenHandle(*v8::Undefined(isolate)) == *undefined_value);
  i::Handle<i::Object> null_value = factory->null_value();
  CHECK(*v8::Utils::OpenHandle(*v8::Null(isolate)) == *null_value);
  i::Handle<i::Object> true_value = factory->true_value();
  CHECK(*v8::Utils::OpenHandle(*v8::True(isolate)) == *true_value);
  i::Handle<i::Object> false_value = factory->false_value();
  CHECK(*v8::Utils::OpenHandle(*v8::False(isolate)) == *false_value);
}


UNINITIALIZED_TEST(IsolateEmbedderData) {
  CcTest::DisableAutomaticDispose();
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  isolate->Enter();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    CHECK(!isolate->GetData(slot));
    CHECK(!i_isolate->GetData(slot));
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xACCE55ED + slot);
    isolate->SetData(slot, data);
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xACCE55ED + slot);
    CHECK_EQ(data, isolate->GetData(slot));
    CHECK_EQ(data, i_isolate->GetData(slot));
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xDECEA5ED + slot);
    isolate->SetData(slot, data);
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xDECEA5ED + slot);
    CHECK_EQ(data, isolate->GetData(slot));
    CHECK_EQ(data, i_isolate->GetData(slot));
  }
  isolate->Exit();
  isolate->Dispose();
}


TEST(StringEmpty) {
  LocalContext context;
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  i::Handle<i::Object> empty_string = factory->empty_string();
  CHECK(*v8::Utils::OpenHandle(*v8::String::Empty(isolate)) == *empty_string);
}


static int instance_checked_getter_count = 0;
static void InstanceCheckedGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
            .FromJust());
  instance_checked_getter_count++;
  info.GetReturnValue().Set(v8_num(11));
}


static int instance_checked_setter_count = 0;
static void InstanceCheckedSetter(Local<String> name,
                      Local<Value> value,
                      const v8::PropertyCallbackInfo<void>& info) {
  CHECK(name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("foo"))
            .FromJust());
  CHECK(value->Equals(info.GetIsolate()->GetCurrentContext(), v8_num(23))
            .FromJust());
  instance_checked_setter_count++;
}


static void CheckInstanceCheckedResult(int getters, int setters,
                                       bool expects_callbacks,
                                       TryCatch* try_catch) {
  if (expects_callbacks) {
    CHECK(!try_catch->HasCaught());
    CHECK_EQ(getters, instance_checked_getter_count);
    CHECK_EQ(setters, instance_checked_setter_count);
  } else {
    CHECK(try_catch->HasCaught());
    CHECK_EQ(0, instance_checked_getter_count);
    CHECK_EQ(0, instance_checked_setter_count);
  }
  try_catch->Reset();
}


static void CheckInstanceCheckedAccessors(bool expects_callbacks) {
  instance_checked_getter_count = 0;
  instance_checked_setter_count = 0;
  TryCatch try_catch(CcTest::isolate());

  // Test path through generic runtime code.
  CompileRun("obj.foo");
  CheckInstanceCheckedResult(1, 0, expects_callbacks, &try_catch);
  CompileRun("obj.foo = 23");
  CheckInstanceCheckedResult(1, 1, expects_callbacks, &try_catch);

  // Test path through generated LoadIC and StoredIC.
  CompileRun(
      "function test_get(o) { o.foo; };"
      "%PrepareFunctionForOptimization(test_get);"
      "test_get(obj);");
  CheckInstanceCheckedResult(2, 1, expects_callbacks, &try_catch);
  CompileRun("test_get(obj);");
  CheckInstanceCheckedResult(3, 1, expects_callbacks, &try_catch);
  CompileRun("test_get(obj);");
  CheckInstanceCheckedResult(4, 1, expects_callbacks, &try_catch);
  CompileRun(
      "function test_set(o) { o.foo = 23; }"
      "%PrepareFunctionForOptimization(test_set);"
      "test_set(obj);");
  CheckInstanceCheckedResult(4, 2, expects_callbacks, &try_catch);
  CompileRun("test_set(obj);");
  CheckInstanceCheckedResult(4, 3, expects_callbacks, &try_catch);
  CompileRun("test_set(obj);");
  CheckInstanceCheckedResult(4, 4, expects_callbacks, &try_catch);

  // Test path through optimized code.
  CompileRun("%OptimizeFunctionOnNextCall(test_get);"
             "test_get(obj);");
  CheckInstanceCheckedResult(5, 4, expects_callbacks, &try_catch);
  CompileRun("%OptimizeFunctionOnNextCall(test_set);"
             "test_set(obj);");
  CheckInstanceCheckedResult(5, 5, expects_callbacks, &try_catch);

  // Cleanup so that closures start out fresh in next check.
  CompileRun(
      "%DeoptimizeFunction(test_get);"
      "%ClearFunctionFeedback(test_get);"
      "%DeoptimizeFunction(test_set);"
      "%ClearFunctionFeedback(test_set);");
}


THREADED_TEST(InstanceCheckOnInstanceAccessor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> inst = templ->InstanceTemplate();
  inst->SetAccessor(v8_str("foo"), InstanceCheckedGetter, InstanceCheckedSetter,
                    Local<Value>(), v8::DEFAULT, v8::None,
                    v8::AccessorSignature::New(context->GetIsolate(), templ));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(false);
}

static void EmptyInterceptorGetter(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {}

static void EmptyInterceptorSetter(
    Local<Name> name, Local<Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {}

THREADED_TEST(InstanceCheckOnInstanceAccessorWithInterceptor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> inst = templ->InstanceTemplate();
  templ->InstanceTemplate()->SetHandler(v8::NamedPropertyHandlerConfiguration(
      EmptyInterceptorGetter, EmptyInterceptorSetter));
  inst->SetAccessor(v8_str("foo"), InstanceCheckedGetter, InstanceCheckedSetter,
                    Local<Value>(), v8::DEFAULT, v8::None,
                    v8::AccessorSignature::New(context->GetIsolate(), templ));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(false);
}


THREADED_TEST(InstanceCheckOnPrototypeAccessor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> proto = templ->PrototypeTemplate();
  proto->SetAccessor(v8_str("foo"), InstanceCheckedGetter,
                     InstanceCheckedSetter, Local<Value>(), v8::DEFAULT,
                     v8::None,
                     v8::AccessorSignature::New(context->GetIsolate(), templ));
  CHECK(context->Global()
            ->Set(context.local(), v8_str("f"),
                  templ->GetFunction(context.local()).ToLocalChecked())
            .FromJust());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(false);

  printf("Testing positive with modified prototype chain ...\n");
  CompileRun("var obj = new f();"
             "var pro = {};"
             "pro.__proto__ = obj.__proto__;"
             "obj.__proto__ = pro;");
  CHECK(templ->HasInstance(
      context->Global()->Get(context.local(), v8_str("obj")).ToLocalChecked()));
  CheckInstanceCheckedAccessors(true);
}


TEST(TryFinallyMessage) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  {
    // Test that the original error message is not lost if there is a
    // recursive call into Javascript is done in the finally block, e.g. to
    // initialize an IC. (crbug.com/129171)
    TryCatch try_catch(context->GetIsolate());
    const char* trigger_ic =
        "try {                      \n"
        "  throw new Error('test'); \n"
        "} finally {                \n"
        "  var x = 0;               \n"
        "  x++;                     \n"  // Trigger an IC initialization here.
        "}                          \n";
    CompileRun(trigger_ic);
    CHECK(try_catch.HasCaught());
    Local<Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(2, message->GetLineNumber(context.local()).FromJust());
  }

  {
    // Test that the original exception message is indeed overwritten if
    // a new error is thrown in the finally block.
    TryCatch try_catch(context->GetIsolate());
    const char* throw_again =
        "try {                       \n"
        "  throw new Error('test');  \n"
        "} finally {                 \n"
        "  var x = 0;                \n"
        "  x++;                      \n"
        "  throw new Error('again'); \n"  // This is the new uncaught error.
        "}                           \n";
    CompileRun(throw_again);
    CHECK(try_catch.HasCaught());
    Local<Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(6, message->GetLineNumber(context.local()).FromJust());
  }
}


static void Helper137002(bool do_store,
                         bool polymorphic,
                         bool remove_accessor,
                         bool interceptor) {
  LocalContext context;
  Local<ObjectTemplate> templ = ObjectTemplate::New(context->GetIsolate());
  if (interceptor) {
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(FooGetInterceptor,
                                                            FooSetInterceptor));
  } else {
    templ->SetAccessor(v8_str("foo"),
                       GetterWhichReturns42,
                       SetterWhichSetsYOnThisTo23);
  }
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  // Turn monomorphic on slow object with native accessor, then turn
  // polymorphic, finally optimize to create negative lookup and fail.
  CompileRun(do_store ?
             "function f(x) { x.foo = void 0; }" :
             "function f(x) { return x.foo; }");
  CompileRun("%PrepareFunctionForOptimization(f);");
  CompileRun("obj.y = void 0;");
  if (!interceptor) {
    CompileRun("%OptimizeObjectForAddingMultipleProperties(obj, 1);");
  }
  CompileRun("obj.__proto__ = null;"
             "f(obj); f(obj); f(obj);");
  if (polymorphic) {
    CompileRun("f({});");
  }
  CompileRun("obj.y = void 0;"
             "%OptimizeFunctionOnNextCall(f);");
  if (remove_accessor) {
    CompileRun("delete obj.foo;");
  }
  CompileRun("var result = f(obj);");
  if (do_store) {
    CompileRun("result = obj.y;");
  }
  if (remove_accessor && !interceptor) {
    CHECK(context->Global()
              ->Get(context.local(), v8_str("result"))
              .ToLocalChecked()
              ->IsUndefined());
  } else {
    CHECK_EQ(do_store ? 23 : 42, context->Global()
                                     ->Get(context.local(), v8_str("result"))
                                     .ToLocalChecked()
                                     ->Int32Value(context.local())
                                     .FromJust());
  }
}


THREADED_TEST(Regress137002a) {
  i::FLAG_allow_natives_syntax = true;
  i::FLAG_compilation_cache = false;
  v8::HandleScope scope(CcTest::isolate());
  for (int i = 0; i < 16; i++) {
    Helper137002(i & 8, i & 4, i & 2, i & 1);
  }
}


THREADED_TEST(Regress137002b) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("foo"),
                     GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  // Turn monomorphic on slow object with native accessor, then just
  // delete the property and fail.
  CompileRun("function load(x) { return x.foo; }"
             "function store(x) { x.foo = void 0; }"
             "function keyed_load(x, key) { return x[key]; }"
             // Second version of function has a different source (add void 0)
             // so that it does not share code with the first version.  This
             // ensures that the ICs are monomorphic.
             "function load2(x) { void 0; return x.foo; }"
             "function store2(x) { void 0; x.foo = void 0; }"
             "function keyed_load2(x, key) { void 0; return x[key]; }"

             "obj.y = void 0;"
             "obj.__proto__ = null;"
             "var subobj = {};"
             "subobj.y = void 0;"
             "subobj.__proto__ = obj;"
             "%OptimizeObjectForAddingMultipleProperties(obj, 1);"

             // Make the ICs monomorphic.
             "load(obj); load(obj);"
             "load2(subobj); load2(subobj);"
             "store(obj); store(obj);"
             "store2(subobj); store2(subobj);"
             "keyed_load(obj, 'foo'); keyed_load(obj, 'foo');"
             "keyed_load2(subobj, 'foo'); keyed_load2(subobj, 'foo');"

             // Actually test the shiny new ICs and better not crash. This
             // serves as a regression test for issue 142088 as well.
             "load(obj);"
             "load2(subobj);"
             "store(obj);"
             "store2(subobj);"
             "keyed_load(obj, 'foo');"
             "keyed_load2(subobj, 'foo');"

             // Delete the accessor.  It better not be called any more now.
             "delete obj.foo;"
             "obj.y = void 0;"
             "subobj.y = void 0;"

             "var load_result = load(obj);"
             "var load_result2 = load2(subobj);"
             "var keyed_load_result = keyed_load(obj, 'foo');"
             "var keyed_load_result2 = keyed_load2(subobj, 'foo');"
             "store(obj);"
             "store2(subobj);"
             "var y_from_obj = obj.y;"
             "var y_from_subobj = subobj.y;");
  CHECK(context->Global()
            ->Get(context.local(), v8_str("load_result"))
            .ToLocalChecked()
            ->IsUndefined());
  CHECK(context->Global()
            ->Get(context.local(), v8_str("load_result2"))
            .ToLocalChecked()
            ->IsUndefined());
  CHECK(context->Global()
            ->Get(context.local(), v8_str("keyed_load_result"))
            .ToLocalChecked()
            ->IsUndefined());
  CHECK(context->Global()
            ->Get(context.local(), v8_str("keyed_load_result2"))
            .ToLocalChecked()
            ->IsUndefined());
  CHECK(context->Global()
            ->Get(context.local(), v8_str("y_from_obj"))
            .ToLocalChecked()
            ->IsUndefined());
  CHECK(context->Global()
            ->Get(context.local(), v8_str("y_from_subobj"))
            .ToLocalChecked()
            ->IsUndefined());
}


THREADED_TEST(Regress142088) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("foo"),
                     GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("obj"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());

  CompileRun("function load(x) { return x.foo; }"
             "var o = Object.create(obj);"
             "%OptimizeObjectForAddingMultipleProperties(obj, 1);"
             "load(o); load(o); load(o); load(o);");
}


THREADED_TEST(Regress137496) {
  i::FLAG_expose_gc = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Compile a try-finally clause where the finally block causes a GC
  // while there still is a message pending for external reporting.
  TryCatch try_catch(context->GetIsolate());
  try_catch.SetVerbose(true);
  CompileRun("try { throw new Error(); } finally { gc(); }");
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(Regress157124) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  Local<Object> obj = templ->NewInstance(context.local()).ToLocalChecked();
  obj->GetIdentityHash();
  obj->DeletePrivate(context.local(),
                     v8::Private::ForApi(isolate, v8_str("Bug")))
      .FromJust();
}


THREADED_TEST(Regress2535) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<Value> set_value = CompileRun("new Set();");
  Local<Object> set_object(Local<Object>::Cast(set_value));
  CHECK_EQ(0, set_object->InternalFieldCount());
  Local<Value> map_value = CompileRun("new Map();");
  Local<Object> map_object(Local<Object>::Cast(map_value));
  CHECK_EQ(0, map_object->InternalFieldCount());
}


THREADED_TEST(Regress2746) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> obj = Object::New(isolate);
  Local<v8::Private> key = v8::Private::New(isolate, v8_str("key"));
  CHECK(
      obj->SetPrivate(context.local(), key, v8::Undefined(isolate)).FromJust());
  Local<Value> value = obj->GetPrivate(context.local(), key).ToLocalChecked();
  CHECK(!value.IsEmpty());
  CHECK(value->IsUndefined());
}


THREADED_TEST(Regress260106) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<FunctionTemplate> templ = FunctionTemplate::New(isolate,
                                                        DummyCallHandler);
  CompileRun("for (var i = 0; i < 128; i++) Object.prototype[i] = 0;");
  Local<Function> function =
      templ->GetFunction(context.local()).ToLocalChecked();
  CHECK(!function.IsEmpty());
  CHECK(function->IsFunction());
}

THREADED_TEST(JSONParseObject) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> obj =
      v8::JSON::Parse(context.local(), v8_str("{\"x\":42}")).ToLocalChecked();
  Local<Object> global = context->Global();
  global->Set(context.local(), v8_str("obj"), obj).FromJust();
  ExpectString("JSON.stringify(obj)", "{\"x\":42}");
}

THREADED_TEST(JSONParseNumber) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> obj =
      v8::JSON::Parse(context.local(), v8_str("42")).ToLocalChecked();
  Local<Object> global = context->Global();
  global->Set(context.local(), v8_str("obj"), obj).FromJust();
  ExpectString("JSON.stringify(obj)", "42");
}

namespace {
void TestJSONParseArray(Local<Context> context, const char* input_str,
                        const char* expected_output_str,
                        i::ElementsKind expected_elements_kind) {
  Local<Value> obj =
      v8::JSON::Parse(context, v8_str(input_str)).ToLocalChecked();

  i::Handle<i::JSArray> a =
      i::Handle<i::JSArray>::cast(v8::Utils::OpenHandle(*obj));
  CHECK_EQ(expected_elements_kind, a->GetElementsKind());

  Local<Object> global = context->Global();
  global->Set(context, v8_str("obj"), obj).FromJust();
  ExpectString("JSON.stringify(obj)", expected_output_str);
}
}  // namespace

THREADED_TEST(JSONParseArray) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());

  TestJSONParseArray(context.local(), "[0, 1, 2]", "[0,1,2]",
                     i::PACKED_SMI_ELEMENTS);
  TestJSONParseArray(context.local(), "[0, 1.2, 2]", "[0,1.2,2]",
                     i::PACKED_DOUBLE_ELEMENTS);
  TestJSONParseArray(context.local(), "[0.2, 1, 2]", "[0.2,1,2]",
                     i::PACKED_DOUBLE_ELEMENTS);
  TestJSONParseArray(context.local(), "[0, \"a\", 2]", "[0,\"a\",2]",
                     i::PACKED_ELEMENTS);
  TestJSONParseArray(context.local(), "[\"a\", 1, 2]", "[\"a\",1,2]",
                     i::PACKED_ELEMENTS);
  TestJSONParseArray(context.local(), "[\"a\", 1.2, 2]", "[\"a\",1.2,2]",
                     i::PACKED_ELEMENTS);
  TestJSONParseArray(context.local(), "[0, 1.2, \"a\"]", "[0,1.2,\"a\"]",
                     i::PACKED_ELEMENTS);
}

THREADED_TEST(JSONStringifyObject) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> value =
      v8::JSON::Parse(context.local(), v8_str("{\"x\":42}")).ToLocalChecked();
  Local<Object> obj = value->ToObject(context.local()).ToLocalChecked();
  Local<Object> global = context->Global();
  global->Set(context.local(), v8_str("obj"), obj).FromJust();
  Local<String> json =
      v8::JSON::Stringify(context.local(), obj).ToLocalChecked();
  v8::String::Utf8Value utf8(context->GetIsolate(), json);
  ExpectString("JSON.stringify(obj)", *utf8);
}

THREADED_TEST(JSONStringifyObjectWithGap) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> value =
      v8::JSON::Parse(context.local(), v8_str("{\"x\":42}")).ToLocalChecked();
  Local<Object> obj = value->ToObject(context.local()).ToLocalChecked();
  Local<Object> global = context->Global();
  global->Set(context.local(), v8_str("obj"), obj).FromJust();
  Local<String> json =
      v8::JSON::Stringify(context.local(), obj, v8_str("*")).ToLocalChecked();
  v8::String::Utf8Value utf8(context->GetIsolate(), json);
  ExpectString("JSON.stringify(obj, null,  '*')", *utf8);
}

#if V8_OS_POSIX
class ThreadInterruptTest {
 public:
  ThreadInterruptTest() : sem_(0), sem_value_(0) { }
  ~ThreadInterruptTest() = default;

  void RunTest() {
    InterruptThread i_thread(this);
    CHECK(i_thread.Start());

    sem_.Wait();
    CHECK_EQ(kExpectedValue, sem_value_);
  }

 private:
  static const int kExpectedValue = 1;

  class InterruptThread : public v8::base::Thread {
   public:
    explicit InterruptThread(ThreadInterruptTest* test)
        : Thread(Options("InterruptThread")), test_(test) {}

    void Run() override {
      struct sigaction action;

      // Ensure that we'll enter waiting condition
      v8::base::OS::Sleep(v8::base::TimeDelta::FromMilliseconds(100));

      // Setup signal handler
      memset(&action, 0, sizeof(action));
      action.sa_handler = SignalHandler;
      sigaction(SIGCHLD, &action, nullptr);

      // Send signal
      kill(getpid(), SIGCHLD);

      // Ensure that if wait has returned because of error
      v8::base::OS::Sleep(v8::base::TimeDelta::FromMilliseconds(100));

      // Set value and signal semaphore
      test_->sem_value_ = 1;
      test_->sem_.Signal();
    }

    static void SignalHandler(int signal) {
    }

   private:
     ThreadInterruptTest* test_;
  };

  v8::base::Semaphore sem_;
  volatile int sem_value_;
};


THREADED_TEST(SemaphoreInterruption) {
  ThreadInterruptTest().RunTest();
}


#endif  // V8_OS_POSIX


void UnreachableCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  UNREACHABLE();
}


TEST(JSONStringifyAccessCheck) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallback(AccessAlwaysBlocked);

  // Create a context and set an x property on it's global object.
  LocalContext context0(nullptr, global_template);
  v8::Local<v8::Object> global0 = context0->Global();
  global0->Set(context0.local(), v8_str("x"), v8_num(42)).FromJust();
  ExpectString("JSON.stringify(this)", "{\"x\":42}");

  for (int i = 0; i < 2; i++) {
    if (i == 1) {
      // Install a toJSON function on the second run.
      v8::Local<v8::FunctionTemplate> toJSON =
          v8::FunctionTemplate::New(isolate, UnreachableCallback);

      global0->Set(context0.local(), v8_str("toJSON"),
                   toJSON->GetFunction(context0.local()).ToLocalChecked())
          .FromJust();
    }
    // Create a context with a different security token so that the
    // failed access check callback will be called on each access.
    LocalContext context1(nullptr, global_template);
    CHECK(context1->Global()
              ->Set(context1.local(), v8_str("other"), global0)
              .FromJust());

    CHECK(CompileRun("JSON.stringify(other)").IsEmpty());
    CHECK(CompileRun("JSON.stringify({ 'a' : other, 'b' : ['c'] })").IsEmpty());
    CHECK(CompileRun("JSON.stringify([other, 'b', 'c'])").IsEmpty());
  }
}


bool access_check_fail_thrown = false;
bool catch_callback_called = false;


// Failed access check callback that performs a GC on each invocation.
void FailedAccessCheckThrows(Local<v8::Object> target,
                             v8::AccessType type,
                             Local<v8::Value> data) {
  access_check_fail_thrown = true;
  i::PrintF("Access check failed. Error thrown.\n");
  CcTest::isolate()->ThrowException(
      v8::Exception::Error(v8_str("cross context")));
}


void CatcherCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  for (int i = 0; i < args.Length(); i++) {
    i::PrintF("%s\n", *String::Utf8Value(args.GetIsolate(), args[i]));
  }
  catch_callback_called = true;
}


void HasOwnPropertyCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
  CHECK(
      args[0]
          ->ToObject(context)
          .ToLocalChecked()
          ->HasOwnProperty(context, args[1]->ToString(context).ToLocalChecked())
          .IsNothing());
}


void CheckCorrectThrow(const char* script) {
  // Test that the script, when wrapped into a try-catch, triggers the catch
  // clause due to failed access check throwing an exception.
  // The subsequent try-catch should run without any exception.
  access_check_fail_thrown = false;
  catch_callback_called = false;
  i::ScopedVector<char> source(1024);
  i::SNPrintF(source, "try { %s; } catch (e) { catcher(e); }", script);
  CompileRun(source.begin());
  CHECK(access_check_fail_thrown);
  CHECK(catch_callback_called);

  access_check_fail_thrown = false;
  catch_callback_called = false;
  CompileRun("try { [1, 2, 3].sort(); } catch (e) { catcher(e) };");
  CHECK(!access_check_fail_thrown);
  CHECK(!catch_callback_called);
}


TEST(AccessCheckThrows) {
  i::FLAG_allow_natives_syntax = true;
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  isolate->SetFailedAccessCheckCallbackFunction(&FailedAccessCheckThrows);
  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallback(AccessAlwaysBlocked);

  // Create a context and set an x property on it's global object.
  LocalContext context0(nullptr, global_template);
  v8::Local<v8::Object> global0 = context0->Global();
  CHECK(global0->Set(context0.local(), v8_str("x"), global0).FromJust());

  // Create a context with a different security token so that the
  // failed access check callback will be called on each access.
  LocalContext context1(nullptr, global_template);
  CHECK(context1->Global()
            ->Set(context1.local(), v8_str("other"), global0)
            .FromJust());

  v8::Local<v8::FunctionTemplate> catcher_fun =
      v8::FunctionTemplate::New(isolate, CatcherCallback);
  CHECK(context1->Global()
            ->Set(context1.local(), v8_str("catcher"),
                  catcher_fun->GetFunction(context1.local()).ToLocalChecked())
            .FromJust());

  v8::Local<v8::FunctionTemplate> has_own_property_fun =
      v8::FunctionTemplate::New(isolate, HasOwnPropertyCallback);
  CHECK(context1->Global()
            ->Set(context1.local(), v8_str("has_own_property"),
                  has_own_property_fun->GetFunction(context1.local())
                      .ToLocalChecked())
            .FromJust());

  {
    v8::TryCatch try_catch(isolate);
    access_check_fail_thrown = false;
    CompileRun("other.x;");
    CHECK(access_check_fail_thrown);
    CHECK(try_catch.HasCaught());
  }

  CheckCorrectThrow("other.x");
  CheckCorrectThrow("other[1]");
  CheckCorrectThrow("JSON.stringify(other)");
  CheckCorrectThrow("has_own_property(other, 'x')");
  CheckCorrectThrow("%GetProperty(other, 'x')");
  CheckCorrectThrow("%SetKeyedProperty(other, 'x', 'foo')");
  CheckCorrectThrow("%SetNamedProperty(other, 'y', 'foo')");
  STATIC_ASSERT(static_cast<int>(i::LanguageMode::kSloppy) == 0);
  STATIC_ASSERT(static_cast<int>(i::LanguageMode::kStrict) == 1);
  CheckCorrectThrow("%DeleteProperty(other, 'x', 0)");  // 0 == SLOPPY
  CheckCorrectThrow("%DeleteProperty(other, 'x', 1)");  // 1 == STRICT
  CheckCorrectThrow("%DeleteProperty(other, '1', 0)");
  CheckCorrectThrow("%DeleteProperty(other, '1', 1)");
  CheckCorrectThrow("Object.prototype.hasOwnProperty.call(other, 'x')");
  CheckCorrectThrow("%HasProperty(other, 'x')");
  CheckCorrectThrow("Object.prototype.propertyIsEnumerable(other, 'x')");
  // PROPERTY_ATTRIBUTES_NONE = 0
  CheckCorrectThrow("%DefineAccessorPropertyUnchecked("
                        "other, 'x', null, null, 1)");

  // Reset the failed access check callback so it does not influence
  // the other tests.
  isolate->SetFailedAccessCheckCallbackFunction(nullptr);
}

namespace {

const char kOneByteSubjectString[] = {
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', '\0'};
const uint16_t kTwoByteSubjectString[] = {
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
    'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', '\0'};

const int kSubjectStringLength = arraysize(kOneByteSubjectString) - 1;
STATIC_ASSERT(arraysize(kOneByteSubjectString) ==
              arraysize(kTwoByteSubjectString));

OneByteVectorResource one_byte_string_resource(
    i::Vector<const char>(&kOneByteSubjectString[0], kSubjectStringLength));
UC16VectorResource two_byte_string_resource(
    i::Vector<const i::uc16>(&kTwoByteSubjectString[0], kSubjectStringLength));

class RegExpInterruptTest {
 public:
  RegExpInterruptTest()
      : i_thread(this),
        env_(),
        isolate_(env_->GetIsolate()),
        sem_(0),
        ran_test_body_(false),
        ran_to_completion_(false) {}

  void RunTest(v8::InterruptCallback test_body_fn) {
    v8::HandleScope handle_scope(isolate_);

    i_thread.SetTestBody(test_body_fn);
    CHECK(i_thread.Start());

    TestBody();

    i_thread.Join();
  }

  static void CollectAllGarbage(v8::Isolate* isolate, void* data) {
    i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
    i_isolate->heap()->PreciseCollectAllGarbage(
        i::Heap::kNoGCFlags, i::GarbageCollectionReason::kRuntime);
  }

  static void MakeSubjectOneByteExternal(v8::Isolate* isolate, void* data) {
    auto instance = reinterpret_cast<RegExpInterruptTest*>(data);

    v8::HandleScope scope(isolate);
    v8::Local<v8::String> string =
        v8::Local<v8::String>::New(isolate, instance->string_handle_);
    CHECK(string->CanMakeExternal());
    string->MakeExternal(&one_byte_string_resource);
  }

  static void MakeSubjectTwoByteExternal(v8::Isolate* isolate, void* data) {
    auto instance = reinterpret_cast<RegExpInterruptTest*>(data);

    v8::HandleScope scope(isolate);
    v8::Local<v8::String> string =
        v8::Local<v8::String>::New(isolate, instance->string_handle_);
    CHECK(string->CanMakeExternal());
    string->MakeExternal(&two_byte_string_resource);
  }

 private:
  static void SignalSemaphore(v8::Isolate* isolate, void* data) {
    reinterpret_cast<RegExpInterruptTest*>(data)->sem_.Signal();
  }

  void CreateTestStrings() {
    i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate_);

    // The string must be in old space to support externalization.
    i::Handle<i::String> i_string =
        i_isolate->factory()->NewStringFromAsciiChecked(
            &kOneByteSubjectString[0], i::AllocationType::kOld);
    v8::Local<v8::String> string = v8::Utils::ToLocal(i_string);

    env_->Global()->Set(env_.local(), v8_str("a"), string).FromJust();

    string_handle_.Reset(env_->GetIsolate(), string);
  }

  void TestBody() {
    CHECK(!ran_test_body_.load());
    CHECK(!ran_to_completion_.load());

    CreateTestStrings();

    v8::TryCatch try_catch(env_->GetIsolate());

    isolate_->RequestInterrupt(&SignalSemaphore, this);
    CompileRun("/((a*)*)*b/.exec(a)");

    CHECK(try_catch.HasTerminated());
    CHECK(ran_test_body_.load());
    CHECK(ran_to_completion_.load());
  }

  class InterruptThread : public v8::base::Thread {
   public:
    explicit InterruptThread(RegExpInterruptTest* test)
        : Thread(Options("RegExpInterruptTest")), test_(test) {}

    void Run() override {
      CHECK_NOT_NULL(test_body_fn_);

      // Wait for JS execution to start.
      test_->sem_.Wait();

      // Sleep for a bit to allow irregexp execution to start up, then run the
      // test body.
      v8::base::OS::Sleep(v8::base::TimeDelta::FromMilliseconds(50));
      test_->isolate_->RequestInterrupt(&RunTestBody, test_);
      test_->isolate_->RequestInterrupt(&SignalSemaphore, test_);

      // Wait for the scheduled interrupt to signal.
      test_->sem_.Wait();

      // Sleep again to resume irregexp execution, then terminate.
      v8::base::OS::Sleep(v8::base::TimeDelta::FromMilliseconds(50));
      test_->ran_to_completion_.store(true);
      test_->isolate_->TerminateExecution();
    }

    static void RunTestBody(v8::Isolate* isolate, void* data) {
      auto instance = reinterpret_cast<RegExpInterruptTest*>(data);
      instance->i_thread.test_body_fn_(isolate, data);
      instance->ran_test_body_.store(true);
    }

    void SetTestBody(v8::InterruptCallback callback) {
      test_body_fn_ = callback;
    }

   private:
    v8::InterruptCallback test_body_fn_;
    RegExpInterruptTest* test_;
  };

  InterruptThread i_thread;

  LocalContext env_;
  v8::Isolate* isolate_;
  v8::base::Semaphore sem_;  // Coordinates between main and interrupt threads.

  v8::Persistent<v8::String> string_handle_;

  std::atomic<bool> ran_test_body_;
  std::atomic<bool> ran_to_completion_;
};

}  // namespace

TEST(RegExpInterruptAndCollectAllGarbage) {
  i::FLAG_always_compact = true;  // Move all movable objects on GC.
  RegExpInterruptTest test;
  test.RunTest(RegExpInterruptTest::CollectAllGarbage);
}

TEST(RegExpInterruptAndMakeSubjectOneByteExternal) {
  RegExpInterruptTest test;
  test.RunTest(RegExpInterruptTest::MakeSubjectOneByteExternal);
}

TEST(RegExpInterruptAndMakeSubjectTwoByteExternal) {
  RegExpInterruptTest test;
  test.RunTest(RegExpInterruptTest::MakeSubjectTwoByteExternal);
}

class RequestInterruptTestBase {
 public:
  RequestInterruptTestBase()
      : env_(),
        isolate_(env_->GetIsolate()),
        sem_(0),
        warmup_(20000),
        should_continue_(true) {
  }

  virtual ~RequestInterruptTestBase() = default;

  virtual void StartInterruptThread() = 0;

  virtual void TestBody() = 0;

  void RunTest() {
    StartInterruptThread();

    v8::HandleScope handle_scope(isolate_);

    TestBody();

    // Verify we arrived here because interruptor was called
    // not due to a bug causing us to exit the loop too early.
    CHECK(!should_continue());
  }

  void WakeUpInterruptor() {
    sem_.Signal();
  }

  bool should_continue() const { return should_continue_; }

  bool ShouldContinue() {
    if (warmup_ > 0) {
      if (--warmup_ == 0) {
        WakeUpInterruptor();
      }
    }

    return should_continue_;
  }

  static void ShouldContinueCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->ShouldContinue());
  }

  LocalContext env_;
  v8::Isolate* isolate_;
  v8::base::Semaphore sem_;
  int warmup_;
  bool should_continue_;
};


class RequestInterruptTestBaseWithSimpleInterrupt
    : public RequestInterruptTestBase {
 public:
  RequestInterruptTestBaseWithSimpleInterrupt() : i_thread(this) { }

  void StartInterruptThread() override { CHECK(i_thread.Start()); }

 private:
  class InterruptThread : public v8::base::Thread {
   public:
    explicit InterruptThread(RequestInterruptTestBase* test)
        : Thread(Options("RequestInterruptTest")), test_(test) {}

    void Run() override {
      test_->sem_.Wait();
      test_->isolate_->RequestInterrupt(&OnInterrupt, test_);
    }

    static void OnInterrupt(v8::Isolate* isolate, void* data) {
      reinterpret_cast<RequestInterruptTestBase*>(data)->
          should_continue_ = false;
    }

   private:
     RequestInterruptTestBase* test_;
  };

  InterruptThread i_thread;
};


class RequestInterruptTestWithFunctionCall
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    Local<Function> func = Function::New(env_.local(), ShouldContinueCallback,
                                         v8::External::New(isolate_, this))
                               .ToLocalChecked();
    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("ShouldContinue"), func)
              .FromJust());

    CompileRun("while (ShouldContinue()) { }");
  }
};


class RequestInterruptTestWithMethodCall
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->Set(v8_str("shouldContinue"),
               FunctionTemplate::New(isolate_, ShouldContinueCallback,
                                     v8::External::New(isolate_, this)));
    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("Klass"),
                    t->GetFunction(env_.local()).ToLocalChecked())
              .FromJust());

    CompileRun("var obj = new Klass; while (obj.shouldContinue()) { }");
  }
};


class RequestInterruptTestWithAccessor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->SetAccessorProperty(v8_str("shouldContinue"), FunctionTemplate::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this)));
    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("Klass"),
                    t->GetFunction(env_.local()).ToLocalChecked())
              .FromJust());

    CompileRun("var obj = new Klass; while (obj.shouldContinue) { }");
  }
};


class RequestInterruptTestWithNativeAccessor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    t->InstanceTemplate()->SetNativeDataProperty(
        v8_str("shouldContinue"), &ShouldContinueNativeGetter, nullptr,
        v8::External::New(isolate_, this));
    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("Klass"),
                    t->GetFunction(env_.local()).ToLocalChecked())
              .FromJust());

    CompileRun("var obj = new Klass; while (obj.shouldContinue) { }");
  }

 private:
  static void ShouldContinueNativeGetter(
      Local<String> property,
      const v8::PropertyCallbackInfo<v8::Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->ShouldContinue());
  }
};


class RequestInterruptTestWithMethodCallAndInterceptor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->Set(v8_str("shouldContinue"),
               FunctionTemplate::New(isolate_, ShouldContinueCallback,
                                     v8::External::New(isolate_, this)));
    v8::Local<v8::ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetHandler(
        v8::NamedPropertyHandlerConfiguration(EmptyInterceptor));

    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("Klass"),
                    t->GetFunction(env_.local()).ToLocalChecked())
              .FromJust());

    CompileRun("var obj = new Klass; while (obj.shouldContinue()) { }");
  }

 private:
  static void EmptyInterceptor(
      Local<Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {}
};


class RequestInterruptTestWithMathAbs
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  void TestBody() override {
    env_->Global()
        ->Set(env_.local(), v8_str("WakeUpInterruptor"),
              Function::New(env_.local(), WakeUpInterruptorCallback,
                            v8::External::New(isolate_, this))
                  .ToLocalChecked())
        .FromJust();

    env_->Global()
        ->Set(env_.local(), v8_str("ShouldContinue"),
              Function::New(env_.local(), ShouldContinueCallback,
                            v8::External::New(isolate_, this))
                  .ToLocalChecked())
        .FromJust();

    i::FLAG_allow_natives_syntax = true;
    CompileRun(
        "function loopish(o) {"
        "  var pre = 10;"
        "  while (o.abs(1) > 0) {"
        "    if (o.abs(1) >= 0 && !ShouldContinue()) break;"
        "    if (pre > 0) {"
        "      if (--pre === 0) WakeUpInterruptor(o === Math);"
        "    }"
        "  }"
        "};"
        "%PrepareFunctionForOptimization(loopish);"
        "var i = 50;"
        "var obj = {abs: function () { return i-- }, x: null};"
        "delete obj.x;"
        "loopish(obj);"
        "%OptimizeFunctionOnNextCall(loopish);"
        "loopish(Math);");

    i::FLAG_allow_natives_syntax = false;
  }

 private:
  static void WakeUpInterruptorCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    if (!info[0]->BooleanValue(info.GetIsolate())) {
      return;
    }

    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    test->WakeUpInterruptor();
  }

  static void ShouldContinueCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->should_continue());
  }
};


TEST(RequestInterruptTestWithFunctionCall) {
  RequestInterruptTestWithFunctionCall().RunTest();
}


TEST(RequestInterruptTestWithMethodCall) {
  RequestInterruptTestWithMethodCall().RunTest();
}


TEST(RequestInterruptTestWithAccessor) {
  RequestInterruptTestWithAccessor().RunTest();
}


TEST(RequestInterruptTestWithNativeAccessor) {
  RequestInterruptTestWithNativeAccessor().RunTest();
}


TEST(RequestInterruptTestWithMethodCallAndInterceptor) {
  RequestInterruptTestWithMethodCallAndInterceptor().RunTest();
}


TEST(RequestInterruptTestWithMathAbs) {
  RequestInterruptTestWithMathAbs().RunTest();
}


class RequestMultipleInterrupts : public RequestInterruptTestBase {
 public:
  RequestMultipleInterrupts() : i_thread(this), counter_(0) {}

  void StartInterruptThread() override { CHECK(i_thread.Start()); }

  void TestBody() override {
    Local<Function> func = Function::New(env_.local(), ShouldContinueCallback,
                                         v8::External::New(isolate_, this))
                               .ToLocalChecked();
    CHECK(env_->Global()
              ->Set(env_.local(), v8_str("ShouldContinue"), func)
              .FromJust());

    CompileRun("while (ShouldContinue()) { }");
  }

 private:
  class InterruptThread : public v8::base::Thread {
   public:
    enum { NUM_INTERRUPTS = 10 };
    explicit InterruptThread(RequestMultipleInterrupts* test)
        : Thread(Options("RequestInterruptTest")), test_(test) {}

    void Run() override {
      test_->sem_.Wait();
      for (int i = 0; i < NUM_INTERRUPTS; i++) {
        test_->isolate_->RequestInterrupt(&OnInterrupt, test_);
      }
    }

    static void OnInterrupt(v8::Isolate* isolate, void* data) {
      RequestMultipleInterrupts* test =
          reinterpret_cast<RequestMultipleInterrupts*>(data);
      test->should_continue_ = ++test->counter_ < NUM_INTERRUPTS;
    }

   private:
    RequestMultipleInterrupts* test_;
  };

  InterruptThread i_thread;
  int counter_;
};


TEST(RequestMultipleInterrupts) { RequestMultipleInterrupts().RunTest(); }


static bool interrupt_was_called = false;


void SmallScriptsInterruptCallback(v8::Isolate* isolate, void* data) {
  interrupt_was_called = true;
}


TEST(RequestInterruptSmallScripts) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  interrupt_was_called = false;
  isolate->RequestInterrupt(&SmallScriptsInterruptCallback, nullptr);
  CompileRun("(function(x){return x;})(1);");
  CHECK(interrupt_was_called);
}


static Local<Value> function_new_expected_env;
static void FunctionNewCallback(const v8::FunctionCallbackInfo<Value>& info) {
  CHECK(
      function_new_expected_env->Equals(info.GetIsolate()->GetCurrentContext(),
                                        info.Data())
          .FromJust());
  info.GetReturnValue().Set(17);
}


THREADED_TEST(FunctionNew) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> data = v8::Object::New(isolate);
  function_new_expected_env = data;
  Local<Function> func =
      Function::New(env.local(), FunctionNewCallback, data).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());
  Local<Value> result = CompileRun("func();");
  CHECK(v8::Integer::New(isolate, 17)->Equals(env.local(), result).FromJust());
  // Serial number should be invalid => should not be cached.
  auto serial_number =
      i::Smi::cast(i::Handle<i::JSFunction>::cast(v8::Utils::OpenHandle(*func))
                       ->shared()
                       .get_api_func_data()
                       .serial_number())
          .value();
  CHECK_EQ(i::FunctionTemplateInfo::kInvalidSerialNumber, serial_number);

  // Verify that each Function::New creates a new function instance
  Local<Object> data2 = v8::Object::New(isolate);
  function_new_expected_env = data2;
  Local<Function> func2 =
      Function::New(env.local(), FunctionNewCallback, data2).ToLocalChecked();
  CHECK(!func2->IsNull());
  CHECK(!func->Equals(env.local(), func2).FromJust());
  CHECK(env->Global()->Set(env.local(), v8_str("func2"), func2).FromJust());
  Local<Value> result2 = CompileRun("func2();");
  CHECK(v8::Integer::New(isolate, 17)->Equals(env.local(), result2).FromJust());
}

namespace {

void Verify(v8::Isolate* isolate, Local<v8::Object> obj) {
#if VERIFY_HEAP
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Handle<i::JSReceiver> i_obj = v8::Utils::OpenHandle(*obj);
  i_obj->ObjectVerify(i_isolate);
#endif
}

}  // namespace

THREADED_TEST(ObjectNew) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  {
    // Verify that Object::New(null) produces an object with a null
    // [[Prototype]].
    Local<v8::Object> obj =
        v8::Object::New(isolate, v8::Null(isolate), nullptr, nullptr, 0);
    CHECK(obj->GetPrototype()->IsNull());
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(0, keys->Length());
  }
  {
    // Verify that Object::New(proto) produces an object with
    // proto as it's [[Prototype]].
    Local<v8::Object> proto = v8::Object::New(isolate);
    Local<v8::Object> obj =
        v8::Object::New(isolate, proto, nullptr, nullptr, 0);
    Verify(isolate, obj);
    CHECK(obj->GetPrototype()->SameValue(proto));
  }
  {
    // Verify that the properties are installed correctly.
    Local<v8::Name> names[3] = {v8_str("a"), v8_str("b"), v8_str("c")};
    Local<v8::Value> values[3] = {v8_num(1), v8_num(2), v8_num(3)};
    Local<v8::Object> obj = v8::Object::New(isolate, v8::Null(isolate), names,
                                            values, arraysize(values));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(arraysize(names), keys->Length());
    for (uint32_t i = 0; i < arraysize(names); ++i) {
      CHECK(names[i]->SameValue(keys->Get(env.local(), i).ToLocalChecked()));
      CHECK(values[i]->SameValue(
          obj->Get(env.local(), names[i]).ToLocalChecked()));
    }
  }
  {
    // Same as above, but with non-null prototype.
    Local<v8::Object> proto = v8::Object::New(isolate);
    Local<v8::Name> names[3] = {v8_str("x"), v8_str("y"), v8_str("z")};
    Local<v8::Value> values[3] = {v8_num(1), v8_num(2), v8_num(3)};
    Local<v8::Object> obj =
        v8::Object::New(isolate, proto, names, values, arraysize(values));
    CHECK(obj->GetPrototype()->SameValue(proto));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(arraysize(names), keys->Length());
    for (uint32_t i = 0; i < arraysize(names); ++i) {
      CHECK(names[i]->SameValue(keys->Get(env.local(), i).ToLocalChecked()));
      CHECK(values[i]->SameValue(
          obj->Get(env.local(), names[i]).ToLocalChecked()));
    }
  }
  {
    // This has to work with duplicate names too.
    Local<v8::Name> names[3] = {v8_str("a"), v8_str("a"), v8_str("a")};
    Local<v8::Value> values[3] = {v8_num(1), v8_num(2), v8_num(3)};
    Local<v8::Object> obj = v8::Object::New(isolate, v8::Null(isolate), names,
                                            values, arraysize(values));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(1, keys->Length());
    CHECK(v8_str("a")->SameValue(keys->Get(env.local(), 0).ToLocalChecked()));
    CHECK(v8_num(3)->SameValue(
        obj->Get(env.local(), v8_str("a")).ToLocalChecked()));
  }
  {
    // This has to work with array indices too.
    Local<v8::Name> names[2] = {v8_str("0"), v8_str("1")};
    Local<v8::Value> values[2] = {v8_num(0), v8_num(1)};
    Local<v8::Object> obj = v8::Object::New(isolate, v8::Null(isolate), names,
                                            values, arraysize(values));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(arraysize(names), keys->Length());
    for (uint32_t i = 0; i < arraysize(names); ++i) {
      CHECK(v8::Number::New(isolate, i)
                ->SameValue(keys->Get(env.local(), i).ToLocalChecked()));
      CHECK(values[i]->SameValue(obj->Get(env.local(), i).ToLocalChecked()));
    }
  }
  {
    // This has to work with mixed array indices / property names too.
    Local<v8::Name> names[2] = {v8_str("0"), v8_str("x")};
    Local<v8::Value> values[2] = {v8_num(42), v8_num(24)};
    Local<v8::Object> obj = v8::Object::New(isolate, v8::Null(isolate), names,
                                            values, arraysize(values));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(arraysize(names), keys->Length());
    // 0 -> 42
    CHECK(v8_num(0)->SameValue(keys->Get(env.local(), 0).ToLocalChecked()));
    CHECK(
        values[0]->SameValue(obj->Get(env.local(), names[0]).ToLocalChecked()));
    // "x" -> 24
    CHECK(v8_str("x")->SameValue(keys->Get(env.local(), 1).ToLocalChecked()));
    CHECK(
        values[1]->SameValue(obj->Get(env.local(), names[1]).ToLocalChecked()));
  }
  {
    // Verify that this also works for a couple thousand properties.
    size_t const kLength = 10 * 1024;
    Local<v8::Name> names[kLength];
    Local<v8::Value> values[kLength];
    for (size_t i = 0; i < arraysize(names); ++i) {
      std::ostringstream ost;
      ost << "a" << i;
      names[i] = v8_str(ost.str().c_str());
      values[i] = v8_num(static_cast<double>(i));
    }
    Local<v8::Object> obj = v8::Object::New(isolate, v8::Null(isolate), names,
                                            values, arraysize(names));
    Verify(isolate, obj);
    Local<Array> keys = obj->GetOwnPropertyNames(env.local()).ToLocalChecked();
    CHECK_EQ(arraysize(names), keys->Length());
    for (uint32_t i = 0; i < arraysize(names); ++i) {
      CHECK(names[i]->SameValue(keys->Get(env.local(), i).ToLocalChecked()));
      CHECK(values[i]->SameValue(
          obj->Get(env.local(), names[i]).ToLocalChecked()));
    }
  }
}

TEST(EscapableHandleScope) {
  HandleScope outer_scope(CcTest::isolate());
  LocalContext context;
  const int runs = 10;
  Local<String> values[runs];
  for (int i = 0; i < runs; i++) {
    v8::EscapableHandleScope inner_scope(CcTest::isolate());
    Local<String> value;
    if (i != 0) value = v8_str("escape value");
    if (i < runs / 2) {
      values[i] = inner_scope.Escape(value);
    } else {
      values[i] = inner_scope.EscapeMaybe(v8::MaybeLocal<String>(value))
                      .ToLocalChecked();
    }
  }
  for (int i = 0; i < runs; i++) {
    Local<String> expected;
    if (i != 0) {
      CHECK(v8_str("escape value")
                ->Equals(context.local(), values[i])
                .FromJust());
    } else {
      CHECK(values[i].IsEmpty());
    }
  }
}


static void SetterWhichExpectsThisAndHolderToDiffer(
    Local<String>, Local<Value>, const v8::PropertyCallbackInfo<void>& info) {
  CHECK(info.Holder() != info.This());
}


TEST(Regress239669) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), nullptr,
                     SetterWhichExpectsThisAndHolderToDiffer);
  CHECK(context->Global()
            ->Set(context.local(), v8_str("P"),
                  templ->NewInstance(context.local()).ToLocalChecked())
            .FromJust());
  CompileRun(
      "function C1() {"
      "  this.x = 23;"
      "};"
      "C1.prototype = P;"
      "for (var i = 0; i < 4; i++ ) {"
      "  new C1();"
      "}");
}


class ApiCallOptimizationChecker {
 private:
  static Local<Object> data;
  static Local<Object> receiver;
  static Local<Object> holder;
  static Local<Object> callee;
  static int count;

  static void OptimizationCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    CHECK(data == info.Data());
    CHECK(receiver == info.This());
    if (info.Length() == 1) {
      CHECK(v8_num(1)
                ->Equals(info.GetIsolate()->GetCurrentContext(), info[0])
                .FromJust());
    }
    CHECK(holder == info.Holder());
    count++;
    info.GetReturnValue().Set(v8_str("returned"));
  }

 public:
  enum SignatureType {
    kNoSignature,
    kSignatureOnReceiver,
    kSignatureOnPrototype
  };

  void RunAll() {
    SignatureType signature_types[] =
      {kNoSignature, kSignatureOnReceiver, kSignatureOnPrototype};
    for (unsigned i = 0; i < arraysize(signature_types); i++) {
      SignatureType signature_type = signature_types[i];
      for (int j = 0; j < 2; j++) {
        bool global = j == 0;
        int key = signature_type +
            arraysize(signature_types) * (global ? 1 : 0);
        Run(signature_type, global, key);
      }
    }
  }

  void Run(SignatureType signature_type, bool global, int key) {
    v8::Isolate* isolate = CcTest::isolate();
    v8::HandleScope scope(isolate);
    // Build a template for signature checks.
    Local<v8::ObjectTemplate> signature_template;
    Local<v8::Signature> signature;
    {
      Local<v8::FunctionTemplate> parent_template =
        FunctionTemplate::New(isolate);
      Local<v8::FunctionTemplate> function_template
          = FunctionTemplate::New(isolate);
      function_template->Inherit(parent_template);
      switch (signature_type) {
        case kNoSignature:
          break;
        case kSignatureOnReceiver:
          signature = v8::Signature::New(isolate, function_template);
          break;
        case kSignatureOnPrototype:
          signature = v8::Signature::New(isolate, parent_template);
          break;
      }
      signature_template = function_template->InstanceTemplate();
    }
    // Global object must pass checks.
    Local<v8::Context> context =
        v8::Context::New(isolate, nullptr, signature_template);
    v8::Context::Scope context_scope(context);
    // Install regular object that can pass signature checks.
    Local<Object> function_receiver =
        signature_template->NewInstance(context).ToLocalChecked();
    CHECK(context->Global()
              ->Set(context, v8_str("function_receiver"), function_receiver)
              .FromJust());
    // Get the holder objects.
    Local<Object> inner_global =
        Local<Object>::Cast(context->Global()->GetPrototype());
    data = Object::New(isolate);
    Local<FunctionTemplate> function_template = FunctionTemplate::New(
        isolate, OptimizationCallback, data, signature);
    Local<Function> function =
        function_template->GetFunction(context).ToLocalChecked();
    Local<Object> global_holder = inner_global;
    Local<Object> function_holder = function_receiver;
    if (signature_type == kSignatureOnPrototype) {
      function_holder = Local<Object>::Cast(function_holder->GetPrototype());
      global_holder = Local<Object>::Cast(global_holder->GetPrototype());
    }
    global_holder->Set(context, v8_str("g_f"), function).FromJust();
    global_holder->SetAccessorProperty(v8_str("g_acc"), function, function);
    function_holder->Set(context, v8_str("f"), function).FromJust();
    function_holder->SetAccessorProperty(v8_str("acc"), function, function);
    // Initialize expected values.
    callee = function;
    count = 0;
    if (global) {
      receiver = context->Global();
      holder = inner_global;
    } else {
      holder = function_receiver;
      // If not using a signature, add something else to the prototype chain
      // to test the case that holder != receiver
      if (signature_type == kNoSignature) {
        receiver = Local<Object>::Cast(CompileRun(
            "var receiver_subclass = {};\n"
            "receiver_subclass.__proto__ = function_receiver;\n"
            "receiver_subclass"));
      } else {
        receiver = Local<Object>::Cast(CompileRun(
          "var receiver_subclass = function_receiver;\n"
          "receiver_subclass"));
      }
    }
    // With no signature, the holder is not set.
    if (signature_type == kNoSignature) holder = receiver;
    // build wrap_function
    i::ScopedVector<char> wrap_function(200);
    if (global) {
      i::SNPrintF(
          wrap_function,
          "function wrap_f_%d() { var f = g_f; return f(); }\n"
          "function wrap_get_%d() { return this.g_acc; }\n"
          "function wrap_set_%d() { return this.g_acc = 1; }\n",
          key, key, key);
    } else {
      i::SNPrintF(
          wrap_function,
          "function wrap_f_%d() { return receiver_subclass.f(); }\n"
          "function wrap_get_%d() { return receiver_subclass.acc; }\n"
          "function wrap_set_%d() { return receiver_subclass.acc = 1; }\n",
          key, key, key);
    }
    // build source string
    i::ScopedVector<char> source(1000);
    i::SNPrintF(source,
                "%s\n"  // wrap functions
                "function wrap_f() { return wrap_f_%d(); }\n"
                "function wrap_get() { return wrap_get_%d(); }\n"
                "function wrap_set() { return wrap_set_%d(); }\n"
                "check = function(returned) {\n"
                "  if (returned !== 'returned') { throw returned; }\n"
                "};\n"
                "\n"
                "%%PrepareFunctionForOptimization(wrap_f_%d);"
                "check(wrap_f());\n"
                "check(wrap_f());\n"
                "%%OptimizeFunctionOnNextCall(wrap_f_%d);\n"
                "check(wrap_f());\n"
                "\n"
                "%%PrepareFunctionForOptimization(wrap_get_%d);"
                "check(wrap_get());\n"
                "check(wrap_get());\n"
                "%%OptimizeFunctionOnNextCall(wrap_get_%d);\n"
                "check(wrap_get());\n"
                "\n"
                "check = function(returned) {\n"
                "  if (returned !== 1) { throw returned; }\n"
                "};\n"
                "%%PrepareFunctionForOptimization(wrap_set_%d);"
                "check(wrap_set());\n"
                "check(wrap_set());\n"
                "%%OptimizeFunctionOnNextCall(wrap_set_%d);\n"
                "check(wrap_set());\n",
                wrap_function.begin(), key, key, key, key, key, key, key, key,
                key);
    v8::TryCatch try_catch(isolate);
    CompileRun(source.begin());
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(9, count);
  }
};


Local<Object> ApiCallOptimizationChecker::data;
Local<Object> ApiCallOptimizationChecker::receiver;
Local<Object> ApiCallOptimizationChecker::holder;
Local<Object> ApiCallOptimizationChecker::callee;
int ApiCallOptimizationChecker::count = 0;


TEST(FunctionCallOptimization) {
  i::FLAG_allow_natives_syntax = true;
  ApiCallOptimizationChecker checker;
  checker.RunAll();
}


TEST(FunctionCallOptimizationMultipleArgs) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();
  Local<v8::Function> function =
      Function::New(context.local(), Returns42).ToLocalChecked();
  global->Set(context.local(), v8_str("x"), function).FromJust();
  CompileRun(
      "function x_wrap() {\n"
      "  for (var i = 0; i < 5; i++) {\n"
      "    x(1,2,3);\n"
      "  }\n"
      "}\n"
      "%PrepareFunctionForOptimization(x_wrap);\n"
      "x_wrap();\n"
      "%OptimizeFunctionOnNextCall(x_wrap);"
      "x_wrap();\n");
}


static void ReturnsSymbolCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8::Symbol::New(info.GetIsolate()));
}


TEST(ApiCallbackCanReturnSymbols) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();
  Local<v8::Function> function =
      Function::New(context.local(), ReturnsSymbolCallback).ToLocalChecked();
  global->Set(context.local(), v8_str("x"), function).FromJust();
  CompileRun(
      "function x_wrap() {\n"
      "  for (var i = 0; i < 5; i++) {\n"
      "    x();\n"
      "  }\n"
      "}\n"
      "%PrepareFunctionForOptimization(x_wrap);\n"
      "x_wrap();\n"
      "%OptimizeFunctionOnNextCall(x_wrap);"
      "x_wrap();\n");
}


TEST(EmptyApiCallback) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto function = FunctionTemplate::New(isolate)
                      ->GetFunction(context.local())
                      .ToLocalChecked();
  global->Set(context.local(), v8_str("x"), function).FromJust();

  auto result = CompileRun("x()");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("x(1,2,3)");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("x.call(undefined)");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("x.call(null)");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("7 + x.call(3) + 11");
  CHECK(result->IsInt32());
  CHECK_EQ(21, result->Int32Value(context.local()).FromJust());

  result = CompileRun("7 + x.call(3, 101, 102, 103, 104) + 11");
  CHECK(result->IsInt32());
  CHECK_EQ(21, result->Int32Value(context.local()).FromJust());

  result = CompileRun("var y = []; x.call(y)");
  CHECK(result->IsArray());

  result = CompileRun("x.call(y, 1, 2, 3, 4)");
  CHECK(result->IsArray());
}


TEST(SimpleSignatureCheck) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto sig_obj = FunctionTemplate::New(isolate);
  auto sig = v8::Signature::New(isolate, sig_obj);
  auto x = FunctionTemplate::New(isolate, Returns42, Local<Value>(), sig);
  global->Set(context.local(), v8_str("sig_obj"),
              sig_obj->GetFunction(context.local()).ToLocalChecked())
      .FromJust();
  global->Set(context.local(), v8_str("x"),
              x->GetFunction(context.local()).ToLocalChecked())
      .FromJust();
  CompileRun("var s = new sig_obj();");
  {
    TryCatch try_catch(isolate);
    CompileRun("x()");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    CompileRun("x.call(1)");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("s.x = x; s.x()");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value(context.local()).FromJust());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("x.call(s)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value(context.local()).FromJust());
  }
}


TEST(ChainSignatureCheck) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto sig_obj = FunctionTemplate::New(isolate);
  auto sig = v8::Signature::New(isolate, sig_obj);
  for (int i = 0; i < 4; ++i) {
    auto temp = FunctionTemplate::New(isolate);
    temp->Inherit(sig_obj);
    sig_obj = temp;
  }
  auto x = FunctionTemplate::New(isolate, Returns42, Local<Value>(), sig);
  global->Set(context.local(), v8_str("sig_obj"),
              sig_obj->GetFunction(context.local()).ToLocalChecked())
      .FromJust();
  global->Set(context.local(), v8_str("x"),
              x->GetFunction(context.local()).ToLocalChecked())
      .FromJust();
  CompileRun("var s = new sig_obj();");
  {
    TryCatch try_catch(isolate);
    CompileRun("x()");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    CompileRun("x.call(1)");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("s.x = x; s.x()");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value(context.local()).FromJust());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("x.call(s)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value(context.local()).FromJust());
  }
}


static const char* last_event_message;
static int last_event_status;
void StoringEventLoggerCallback(const char* message, int status) {
    last_event_message = message;
    last_event_status = status;
}


TEST(EventLogging) {
  v8::Isolate* isolate = CcTest::isolate();
  isolate->SetEventLogger(StoringEventLoggerCallback);
  v8::internal::HistogramTimer histogramTimer(
      "V8.Test", 0, 10000, v8::internal::HistogramTimerResolution::MILLISECOND,
      50, reinterpret_cast<v8::internal::Isolate*>(isolate)->counters());
  histogramTimer.Start();
  CHECK_EQ(0, strcmp("V8.Test", last_event_message));
  CHECK_EQ(0, last_event_status);
  histogramTimer.Stop();
  CHECK_EQ(0, strcmp("V8.Test", last_event_message));
  CHECK_EQ(1, last_event_status);
}

TEST(PropertyDescriptor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  {  // empty descriptor
    v8::PropertyDescriptor desc;
    CHECK(!desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_configurable());
    CHECK(!desc.has_writable());
  }
  {
    // data descriptor
    v8::PropertyDescriptor desc(v8_num(42));
    desc.set_enumerable(false);
    CHECK(desc.value() == v8_num(42));
    CHECK(desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(desc.has_enumerable());
    CHECK(!desc.enumerable());
    CHECK(!desc.has_configurable());
    CHECK(!desc.has_writable());
  }
  {
    // data descriptor
    v8::PropertyDescriptor desc(v8_num(42));
    desc.set_configurable(true);
    CHECK(desc.value() == v8_num(42));
    CHECK(desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(desc.has_configurable());
    CHECK(desc.configurable());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_writable());
  }
  {
    // data descriptor
    v8::PropertyDescriptor desc(v8_num(42));
    desc.set_configurable(false);
    CHECK(desc.value() == v8_num(42));
    CHECK(desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(desc.has_configurable());
    CHECK(!desc.configurable());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_writable());
  }
  {
    // data descriptor
    v8::PropertyDescriptor desc(v8_num(42), false);
    CHECK(desc.value() == v8_num(42));
    CHECK(desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_configurable());
    CHECK(desc.has_writable());
    CHECK(!desc.writable());
  }
  {
    // data descriptor
    v8::PropertyDescriptor desc(v8::Local<v8::Value>(), true);
    CHECK(!desc.has_value());
    CHECK(!desc.has_set());
    CHECK(!desc.has_get());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_configurable());
    CHECK(desc.has_writable());
    CHECK(desc.writable());
  }
  {
    // accessor descriptor
    CompileRun("var set = function() {return 43;};");

    v8::Local<v8::Function> set =
        v8::Local<v8::Function>::Cast(context->Global()
                                          ->Get(context.local(), v8_str("set"))
                                          .ToLocalChecked());
    v8::PropertyDescriptor desc(v8::Undefined(isolate), set);
    desc.set_configurable(false);
    CHECK(!desc.has_value());
    CHECK(desc.has_get());
    CHECK(desc.get() == v8::Undefined(isolate));
    CHECK(desc.has_set());
    CHECK(desc.set() == set);
    CHECK(!desc.has_enumerable());
    CHECK(desc.has_configurable());
    CHECK(!desc.configurable());
    CHECK(!desc.has_writable());
  }
  {
    // accessor descriptor with Proxy
    CompileRun(
        "var set = new Proxy(function() {}, {});"
        "var get = undefined;");

    v8::Local<v8::Value> get =
        v8::Local<v8::Value>::Cast(context->Global()
                                       ->Get(context.local(), v8_str("get"))
                                       .ToLocalChecked());
    v8::Local<v8::Function> set =
        v8::Local<v8::Function>::Cast(context->Global()
                                          ->Get(context.local(), v8_str("set"))
                                          .ToLocalChecked());
    v8::PropertyDescriptor desc(get, set);
    desc.set_configurable(false);
    CHECK(!desc.has_value());
    CHECK(desc.get() == v8::Undefined(isolate));
    CHECK(desc.has_get());
    CHECK(desc.set() == set);
    CHECK(desc.has_set());
    CHECK(!desc.has_enumerable());
    CHECK(desc.has_configurable());
    CHECK(!desc.configurable());
    CHECK(!desc.has_writable());
  }
  {
    // accessor descriptor with empty function handle
    v8::Local<v8::Function> get = v8::Local<v8::Function>();
    v8::PropertyDescriptor desc(get, get);
    CHECK(!desc.has_value());
    CHECK(!desc.has_get());
    CHECK(!desc.has_set());
    CHECK(!desc.has_enumerable());
    CHECK(!desc.has_configurable());
    CHECK(!desc.has_writable());
  }
}

TEST(Promises) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  // Creation.
  Local<v8::Promise::Resolver> pr =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  Local<v8::Promise::Resolver> rr =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  Local<v8::Promise> p = pr->GetPromise();
  Local<v8::Promise> r = rr->GetPromise();

  // IsPromise predicate.
  CHECK(p->IsPromise());
  CHECK(r->IsPromise());
  Local<Value> o = v8::Object::New(isolate);
  CHECK(!o->IsPromise());

  // Resolution and rejection.
  pr->Resolve(context.local(), v8::Integer::New(isolate, 1)).FromJust();
  CHECK(p->IsPromise());
  rr->Reject(context.local(), v8::Integer::New(isolate, 2)).FromJust();
  CHECK(r->IsPromise());
}

// Promise.Then(on_fulfilled)
TEST(PromiseThen) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();

  // Creation.
  Local<v8::Promise::Resolver> pr =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  Local<v8::Promise::Resolver> qr =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  Local<v8::Promise> p = pr->GetPromise();
  Local<v8::Promise> q = qr->GetPromise();

  CHECK(p->IsPromise());
  CHECK(q->IsPromise());

  pr->Resolve(context.local(), v8::Integer::New(isolate, 1)).FromJust();
  qr->Resolve(context.local(), p).FromJust();

  // Chaining non-pending promises.
  CompileRun(
      "var x1 = 0;\n"
      "var x2 = 0;\n"
      "function f1(x) { x1 = x; return x+1 };\n"
      "function f2(x) { x2 = x; return x+1 };\n");
  Local<Function> f1 = Local<Function>::Cast(
      global->Get(context.local(), v8_str("f1")).ToLocalChecked());
  Local<Function> f2 = Local<Function>::Cast(
      global->Get(context.local(), v8_str("f2")).ToLocalChecked());

  // Then
  CompileRun("x1 = x2 = 0;");
  q->Then(context.local(), f1).ToLocalChecked();
  CHECK_EQ(0, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());

  // Then
  CompileRun("x1 = x2 = 0;");
  pr = v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  qr = v8::Promise::Resolver::New(context.local()).ToLocalChecked();

  qr->Resolve(context.local(), pr).FromJust();
  qr->GetPromise()
      ->Then(context.local(), f1)
      .ToLocalChecked()
      ->Then(context.local(), f2)
      .ToLocalChecked();

  CHECK_EQ(0, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(0, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  isolate->RunMicrotasks();
  CHECK_EQ(0, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(0, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());

  pr->Resolve(context.local(), v8::Integer::New(isolate, 3)).FromJust();

  CHECK_EQ(0, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(0, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  isolate->RunMicrotasks();
  CHECK_EQ(3, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(4, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
}

// Promise.Then(on_fulfilled, on_rejected)
TEST(PromiseThen2) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  v8::HandleScope scope(isolate);
  Local<Object> global = context->Global();

  // Creation.
  Local<v8::Promise::Resolver> pr =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  Local<v8::Promise> p = pr->GetPromise();

  CHECK(p->IsPromise());

  pr->Resolve(context.local(), v8::Integer::New(isolate, 1)).FromJust();

  // Chaining non-pending promises.
  CompileRun(
      "var x1 = 0;\n"
      "var x2 = 0;\n"
      "function f1(x) { x1 = x; return x+1 };\n"
      "function f2(x) { x2 = x; return x+1 };\n"
      "function f3(x) { throw x + 100 };\n");
  Local<Function> f1 = Local<Function>::Cast(
      global->Get(context.local(), v8_str("f1")).ToLocalChecked());
  Local<Function> f2 = Local<Function>::Cast(
      global->Get(context.local(), v8_str("f2")).ToLocalChecked());
  Local<Function> f3 = Local<Function>::Cast(
      global->Get(context.local(), v8_str("f3")).ToLocalChecked());

  // Then
  CompileRun("x1 = x2 = 0;");
  Local<v8::Promise> a = p->Then(context.local(), f1, f2).ToLocalChecked();
  CHECK_EQ(0, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(0, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());

  Local<v8::Promise> b = a->Then(context.local(), f3, f2).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(0, global->Get(context.local(), v8_str("x2"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());

  Local<v8::Promise> c = b->Then(context.local(), f1, f2).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(context.local(), v8_str("x1"))
                  .ToLocalChecked()
                  ->Int32Value(context.local())
                  .FromJust());
  CHECK_EQ(102, global->Get(context.local(), v8_str("x2"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());

  v8::Local<v8::Promise> d = c->Then(context.local(), f1, f2).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(103, global->Get(context.local(), v8_str("x1"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());
  CHECK_EQ(102, global->Get(context.local(), v8_str("x2"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());

  v8::Local<v8::Promise> e = d->Then(context.local(), f3, f2).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(103, global->Get(context.local(), v8_str("x1"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());
  CHECK_EQ(102, global->Get(context.local(), v8_str("x2"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());

  v8::Local<v8::Promise> f = e->Then(context.local(), f1, f3).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(103, global->Get(context.local(), v8_str("x1"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());
  CHECK_EQ(102, global->Get(context.local(), v8_str("x2"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());

  f->Then(context.local(), f1, f2).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(103, global->Get(context.local(), v8_str("x1"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());
  CHECK_EQ(304, global->Get(context.local(), v8_str("x2"))
                    .ToLocalChecked()
                    ->Int32Value(context.local())
                    .FromJust());
}

TEST(PromiseStateAndValue) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Value> result = CompileRun(
      "var resolver;"
      "new Promise((res, rej) => { resolver = res; })");
  v8::Local<v8::Promise> promise = v8::Local<v8::Promise>::Cast(result);
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kPending);

  CompileRun("resolver('fulfilled')");
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kFulfilled);
  CHECK(v8_str("fulfilled")->SameValue(promise->Result()));

  result = CompileRun("Promise.reject('rejected')");
  promise = v8::Local<v8::Promise>::Cast(result);
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kRejected);
  CHECK(v8_str("rejected")->SameValue(promise->Result()));
}

TEST(ResolvedPromiseReFulfill) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> value1 =
      v8::String::NewFromUtf8(isolate, "foo", v8::NewStringType::kNormal)
          .ToLocalChecked();
  v8::Local<v8::String> value2 =
      v8::String::NewFromUtf8(isolate, "bar", v8::NewStringType::kNormal)
          .ToLocalChecked();

  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  v8::Local<v8::Promise> promise = resolver->GetPromise();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kPending);

  resolver->Resolve(context.local(), value1).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kFulfilled);
  CHECK_EQ(promise->Result(), value1);

  // This should be a no-op.
  resolver->Resolve(context.local(), value2).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kFulfilled);
  CHECK_EQ(promise->Result(), value1);

  // This should be a no-op.
  resolver->Reject(context.local(), value2).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kFulfilled);
  CHECK_EQ(promise->Result(), value1);
}

TEST(RejectedPromiseReFulfill) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> value1 =
      v8::String::NewFromUtf8(isolate, "foo", v8::NewStringType::kNormal)
          .ToLocalChecked();
  v8::Local<v8::String> value2 =
      v8::String::NewFromUtf8(isolate, "bar", v8::NewStringType::kNormal)
          .ToLocalChecked();

  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context.local()).ToLocalChecked();
  v8::Local<v8::Promise> promise = resolver->GetPromise();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kPending);

  resolver->Reject(context.local(), value1).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kRejected);
  CHECK_EQ(promise->Result(), value1);

  // This should be a no-op.
  resolver->Reject(context.local(), value2).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kRejected);
  CHECK_EQ(promise->Result(), value1);

  // This should be a no-op.
  resolver->Resolve(context.local(), value2).ToChecked();
  CHECK_EQ(promise->State(), v8::Promise::PromiseState::kRejected);
  CHECK_EQ(promise->Result(), value1);
}

TEST(DisallowJavascriptExecutionScope) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope no_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::CRASH_ON_FAILURE);
  CompileRun("2+2");
}

TEST(AllowJavascriptExecutionScope) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope no_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::CRASH_ON_FAILURE);
  v8::Isolate::DisallowJavascriptExecutionScope throw_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::THROW_ON_FAILURE);
  { v8::Isolate::AllowJavascriptExecutionScope yes_js(isolate);
    CompileRun("1+1");
  }
}

TEST(ThrowOnJavascriptExecution) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope throw_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::THROW_ON_FAILURE);
  CompileRun("1+1");
  CHECK(try_catch.HasCaught());
}

namespace {

class MockPlatform : public TestPlatform {
 public:
  MockPlatform() : old_platform_(i::V8::GetCurrentPlatform()) {
    // Now that it's completely constructed, make this the current platform.
    i::V8::SetPlatformForTesting(this);
  }
  ~MockPlatform() override { i::V8::SetPlatformForTesting(old_platform_); }

  bool dump_without_crashing_called() const {
    return dump_without_crashing_called_;
  }

  void DumpWithoutCrashing() override { dump_without_crashing_called_ = true; }

 private:
  v8::Platform* old_platform_;
  bool dump_without_crashing_called_ = false;
};

}  // namespace

TEST(DumpOnJavascriptExecution) {
  MockPlatform platform;

  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope throw_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::DUMP_ON_FAILURE);
  CHECK(!platform.dump_without_crashing_called());
  CompileRun("1+1");
  CHECK(platform.dump_without_crashing_called());
}

TEST(Regress354123) {
  LocalContext current;
  v8::Isolate* isolate = current->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallback(AccessCounter);
  CHECK(current->Global()
            ->Set(current.local(), v8_str("friend"),
                  templ->NewInstance(current.local()).ToLocalChecked())
            .FromJust());

  // Test access using __proto__ from the prototype chain.
  access_count = 0;
  CompileRun("friend.__proto__ = {};");
  CHECK_EQ(2, access_count);
  CompileRun("friend.__proto__;");
  CHECK_EQ(4, access_count);

  // Test access using __proto__ as a hijacked function (A).
  access_count = 0;
  CompileRun("var p = Object.prototype;"
             "var f = Object.getOwnPropertyDescriptor(p, '__proto__').set;"
             "f.call(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("var p = Object.prototype;"
             "var f = Object.getOwnPropertyDescriptor(p, '__proto__').get;"
             "f.call(friend);");
  CHECK_EQ(2, access_count);

  // Test access using __proto__ as a hijacked function (B).
  access_count = 0;
  CompileRun("var f = Object.prototype.__lookupSetter__('__proto__');"
             "f.call(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("var f = Object.prototype.__lookupGetter__('__proto__');"
             "f.call(friend);");
  CHECK_EQ(2, access_count);

  // Test access using Object.setPrototypeOf reflective method.
  access_count = 0;
  CompileRun("Object.setPrototypeOf(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("Object.getPrototypeOf(friend);");
  CHECK_EQ(2, access_count);
}


namespace {
bool ValueEqualsString(v8::Isolate* isolate, Local<Value> lhs,
                       const char* rhs) {
  CHECK(!lhs.IsEmpty());
  CHECK(lhs->IsString());
  String::Utf8Value utf8_lhs(isolate, lhs);
  return strcmp(rhs, *utf8_lhs) == 0;
}
}  // namespace

TEST(ScriptNameAndLineNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  const char* url = "http://www.foo.com/foo.js";
  v8::ScriptOrigin origin(v8_str(url), v8::Integer::New(isolate, 13));
  v8::ScriptCompiler::Source script_source(v8_str("var foo;"), origin);

  Local<Script> script =
      v8::ScriptCompiler::Compile(env.local(), &script_source).ToLocalChecked();
  CHECK(ValueEqualsString(isolate, script->GetUnboundScript()->GetScriptName(),
                          url));

  int line_number = script->GetUnboundScript()->GetLineNumber(0);
  CHECK_EQ(13, line_number);
}

TEST(ScriptPositionInfo) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::HandleScope scope(isolate);
  const char* url = "http://www.foo.com/foo.js";
  v8::ScriptOrigin origin(v8_str(url), v8::Integer::New(isolate, 13));
  v8::ScriptCompiler::Source script_source(v8_str("var foo;\n"
                                                  "var bar;\n"
                                                  "var fisk = foo + bar;\n"),
                                           origin);
  Local<Script> script =
      v8::ScriptCompiler::Compile(env.local(), &script_source).ToLocalChecked();

  i::Handle<i::SharedFunctionInfo> obj = i::Handle<i::SharedFunctionInfo>::cast(
      v8::Utils::OpenHandle(*script->GetUnboundScript()));
  CHECK(obj->script().IsScript());

  i::Handle<i::Script> script1(i::Script::cast(obj->script()), i_isolate);

  v8::internal::Script::PositionInfo info;

  for (int i = 0; i < 2; ++i) {
    // With offset.

    // Behave as if 0 was passed if position is negative.
    CHECK(script1->GetPositionInfo(-1, &info, script1->WITH_OFFSET));
    CHECK_EQ(13, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(0, &info, script1->WITH_OFFSET));
    CHECK_EQ(13, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(8, &info, script1->WITH_OFFSET));
    CHECK_EQ(13, info.line);
    CHECK_EQ(8, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(9, &info, script1->WITH_OFFSET));
    CHECK_EQ(14, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(9, info.line_start);
    CHECK_EQ(17, info.line_end);

    // Fail when position is larger than script size.
    CHECK(!script1->GetPositionInfo(220384, &info, script1->WITH_OFFSET));

    // Without offset.

    // Behave as if 0 was passed if position is negative.
    CHECK(script1->GetPositionInfo(-1, &info, script1->NO_OFFSET));
    CHECK_EQ(0, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(0, &info, script1->NO_OFFSET));
    CHECK_EQ(0, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(8, &info, script1->NO_OFFSET));
    CHECK_EQ(0, info.line);
    CHECK_EQ(8, info.column);
    CHECK_EQ(0, info.line_start);
    CHECK_EQ(8, info.line_end);

    CHECK(script1->GetPositionInfo(9, &info, script1->NO_OFFSET));
    CHECK_EQ(1, info.line);
    CHECK_EQ(0, info.column);
    CHECK_EQ(9, info.line_start);
    CHECK_EQ(17, info.line_end);

    // Fail when position is larger than script size.
    CHECK(!script1->GetPositionInfo(220384, &info, script1->NO_OFFSET));

    i::Script::InitLineEnds(script1);
  }
}

void CheckMagicComments(v8::Isolate* isolate, Local<Script> script,
                        const char* expected_source_url,
                        const char* expected_source_mapping_url) {
  if (expected_source_url != nullptr) {
    v8::String::Utf8Value url(isolate,
                              script->GetUnboundScript()->GetSourceURL());
    CHECK_EQ(0, strcmp(expected_source_url, *url));
  } else {
    CHECK(script->GetUnboundScript()->GetSourceURL()->IsUndefined());
  }
  if (expected_source_mapping_url != nullptr) {
    v8::String::Utf8Value url(
        isolate, script->GetUnboundScript()->GetSourceMappingURL());
    CHECK_EQ(0, strcmp(expected_source_mapping_url, *url));
  } else {
    CHECK(script->GetUnboundScript()->GetSourceMappingURL()->IsUndefined());
  }
}

void SourceURLHelper(v8::Isolate* isolate, const char* source,
                     const char* expected_source_url,
                     const char* expected_source_mapping_url) {
  Local<Script> script = v8_compile(source);
  CheckMagicComments(isolate, script, expected_source_url,
                     expected_source_mapping_url);
}


TEST(ScriptSourceURLAndSourceMappingURL) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar1.js\n",
                  "bar1.js", nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceMappingURL=bar2.js\n",
                  nullptr, "bar2.js");

  // Both sourceURL and sourceMappingURL.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar3.js\n"
                  "//# sourceMappingURL=bar4.js\n",
                  "bar3.js", "bar4.js");

  // Two source URLs; the first one is ignored.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=ignoreme.js\n"
                  "//# sourceURL=bar5.js\n",
                  "bar5.js", nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceMappingURL=ignoreme.js\n"
                  "//# sourceMappingURL=bar6.js\n",
                  nullptr, "bar6.js");

  // SourceURL or sourceMappingURL in the middle of the script.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar7.js\n"
                  "function baz() {}\n",
                  "bar7.js", nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceMappingURL=bar8.js\n"
                  "function baz() {}\n",
                  nullptr, "bar8.js");

  // Too much whitespace.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//#  sourceURL=bar9.js\n"
                  "//#  sourceMappingURL=bar10.js\n",
                  nullptr, nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL =bar11.js\n"
                  "//# sourceMappingURL =bar12.js\n",
                  nullptr, nullptr);

  // Disallowed characters in value.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar13 .js   \n"
                  "//# sourceMappingURL=bar14 .js \n",
                  nullptr, nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar15\t.js   \n"
                  "//# sourceMappingURL=bar16\t.js \n",
                  nullptr, nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar17'.js   \n"
                  "//# sourceMappingURL=bar18'.js \n",
                  nullptr, nullptr);
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=bar19\".js   \n"
                  "//# sourceMappingURL=bar20\".js \n",
                  nullptr, nullptr);

  // Not too much whitespace.
  SourceURLHelper(isolate,
                  "function foo() {}\n"
                  "//# sourceURL=  bar21.js   \n"
                  "//# sourceMappingURL=  bar22.js \n",
                  "bar21.js", "bar22.js");
}


TEST(GetOwnPropertyDescriptor) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "var x = { value : 13};"
      "Object.defineProperty(x, 'p0', {value : 12});"
      "Object.defineProperty(x, Symbol.toStringTag, {value: 'foo'});"
      "Object.defineProperty(x, 'p1', {"
      "  set : function(value) { this.value = value; },"
      "  get : function() { return this.value; },"
      "});");
  Local<Object> x = Local<Object>::Cast(
      env->Global()->Get(env.local(), v8_str("x")).ToLocalChecked());
  Local<Value> desc =
      x->GetOwnPropertyDescriptor(env.local(), v8_str("no_prop"))
          .ToLocalChecked();
  CHECK(desc->IsUndefined());
  desc =
      x->GetOwnPropertyDescriptor(env.local(), v8_str("p0")).ToLocalChecked();
  CHECK(v8_num(12)
            ->Equals(env.local(), Local<Object>::Cast(desc)
                                      ->Get(env.local(), v8_str("value"))
                                      .ToLocalChecked())
            .FromJust());
  desc =
      x->GetOwnPropertyDescriptor(env.local(), v8_str("p1")).ToLocalChecked();
  Local<Function> set =
      Local<Function>::Cast(Local<Object>::Cast(desc)
                                ->Get(env.local(), v8_str("set"))
                                .ToLocalChecked());
  Local<Function> get =
      Local<Function>::Cast(Local<Object>::Cast(desc)
                                ->Get(env.local(), v8_str("get"))
                                .ToLocalChecked());
  CHECK(v8_num(13)
            ->Equals(env.local(),
                     get->Call(env.local(), x, 0, nullptr).ToLocalChecked())
            .FromJust());
  Local<Value> args[] = {v8_num(14)};
  set->Call(env.local(), x, 1, args).ToLocalChecked();
  CHECK(v8_num(14)
            ->Equals(env.local(),
                     get->Call(env.local(), x, 0, nullptr).ToLocalChecked())
            .FromJust());
  desc =
      x->GetOwnPropertyDescriptor(env.local(), Symbol::GetToStringTag(isolate))
          .ToLocalChecked();
  CHECK(v8_str("foo")
            ->Equals(env.local(), Local<Object>::Cast(desc)
                                      ->Get(env.local(), v8_str("value"))
                                      .ToLocalChecked())
            .FromJust());
}


TEST(Regress411877) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(AccessCounter);

  v8::Local<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  CHECK(context->Global()
            ->Set(context, v8_str("o"),
                  object_template->NewInstance(context).ToLocalChecked())
            .FromJust());
  CompileRun("Object.getOwnPropertyNames(o)");
}


TEST(GetHiddenPropertyTableAfterAccessCheck) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(AccessCounter);

  v8::Local<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> obj =
      object_template->NewInstance(context).ToLocalChecked();
  obj->Set(context, v8_str("key"), v8_str("value")).FromJust();
  obj->Delete(context, v8_str("key")).FromJust();

  obj->SetPrivate(context, v8::Private::New(isolate, v8_str("hidden key 2")),
                  v8_str("hidden value 2"))
      .FromJust();
}


TEST(Regress411793) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(AccessCounter);

  v8::Local<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  CHECK(context->Global()
            ->Set(context, v8_str("o"),
                  object_template->NewInstance(context).ToLocalChecked())
            .FromJust());
  CompileRun(
      "Object.defineProperty(o, 'key', "
      "    { get: function() {}, set: function() {} });");
}

class TestSourceStream : public v8::ScriptCompiler::ExternalSourceStream {
 public:
  explicit TestSourceStream(const char** chunks) : chunks_(chunks), index_(0) {}

  size_t GetMoreData(const uint8_t** src) override {
    // Unlike in real use cases, this function will never block.
    if (chunks_[index_] == nullptr) {
      return 0;
    }
    // Copy the data, since the caller takes ownership of it.
    size_t len = strlen(chunks_[index_]);
    // We don't need to zero-terminate since we return the length.
    uint8_t* copy = new uint8_t[len];
    memcpy(copy, chunks_[index_], len);
    *src = copy;
    ++index_;
    return len;
  }

  // Helper for constructing a string from chunks (the compilation needs it
  // too).
  static char* FullSourceString(const char** chunks) {
    size_t total_len = 0;
    for (size_t i = 0; chunks[i] != nullptr; ++i) {
      total_len += strlen(chunks[i]);
    }
    char* full_string = new char[total_len + 1];
    size_t offset = 0;
    for (size_t i = 0; chunks[i] != nullptr; ++i) {
      size_t len = strlen(chunks[i]);
      memcpy(full_string + offset, chunks[i], len);
      offset += len;
    }
    full_string[total_len] = 0;
    return full_string;
  }

 private:
  const char** chunks_;
  unsigned index_;
};


// Helper function for running streaming tests.
void RunStreamingTest(const char** chunks,
                      v8::ScriptCompiler::StreamedSource::Encoding encoding =
                          v8::ScriptCompiler::StreamedSource::ONE_BYTE,
                      bool expected_success = true,
                      const char* expected_source_url = nullptr,
                      const char* expected_source_mapping_url = nullptr) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);

  v8::ScriptCompiler::StreamedSource source(
      v8::base::make_unique<TestSourceStream>(chunks), encoding);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(isolate, &source);

  // TestSourceStream::GetMoreData won't block, so it's OK to just run the
  // task here in the main thread.
  task->Run();
  delete task;

  // Possible errors are only produced while compiling.
  CHECK(!try_catch.HasCaught());

  v8::ScriptOrigin origin(v8_str("http://foo.com"));
  char* full_source = TestSourceStream::FullSourceString(chunks);
  v8::MaybeLocal<Script> script = v8::ScriptCompiler::Compile(
      env.local(), &source, v8_str(full_source), origin);
  if (expected_success) {
    CHECK(!script.IsEmpty());
    v8::Local<Value> result(
        script.ToLocalChecked()->Run(env.local()).ToLocalChecked());
    // All scripts are supposed to return the fixed value 13 when ran.
    CHECK_EQ(13, result->Int32Value(env.local()).FromJust());
    CheckMagicComments(isolate, script.ToLocalChecked(), expected_source_url,
                       expected_source_mapping_url);
  } else {
    CHECK(script.IsEmpty());
    CHECK(try_catch.HasCaught());
  }
  delete[] full_source;
}

TEST(StreamingSimpleScript) {
  // This script is unrealistically small, since no one chunk is enough to fill
  // the backing buffer of Scanner, let alone overflow it.
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo(); ",
                          nullptr};
  RunStreamingTest(chunks);
}

TEST(StreamingScriptConstantArray) {
  // When run with Ignition, tests that the streaming parser canonicalizes
  // handles so that they are only added to the constant pool array once.
  const char* chunks[] = {
      "var a = {};",        "var b = {};", "var c = 'testing';",
      "var d = 'testing';", "13;",         nullptr};
  RunStreamingTest(chunks);
}

TEST(StreamingScriptEvalShadowing) {
  // When run with Ignition, tests that the streaming parser canonicalizes
  // handles so the Variable::is_possibly_eval() is correct.
  const char* chunk1 =
      "(function() {\n"
      "  var y = 2;\n"
      "  return (function() {\n"
      "    eval('var y = 13;');\n"
      "    function g() {\n"
      "      return y\n"
      "    }\n"
      "    return g();\n"
      "  })()\n"
      "})()\n";
  const char* chunks[] = {chunk1, nullptr};
  RunStreamingTest(chunks);
}

TEST(StreamingBiggerScript) {
  const char* chunk1 =
      "function foo() {\n"
      "  // Make this chunk sufficiently long so that it will overflow the\n"
      "  // backing buffer of the Scanner.\n"
      "  var i = 0;\n"
      "  var result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  return result;\n"
      "}\n";
  const char* chunks[] = {chunk1, "foo(); ", nullptr};
  RunStreamingTest(chunks);
}


TEST(StreamingScriptWithParseError) {
  // Test that parse errors from streamed scripts are propagated correctly.
  {
    char chunk1[] =
        "  // This will result in a parse error.\n"
        "  var if else then foo";
    char chunk2[] = "  13\n";
    const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};

    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::ONE_BYTE,
                     false);
  }
  // Test that the next script succeeds normally.
  {
    char chunk1[] =
        "  // This will be parsed successfully.\n"
        "  function foo() { return ";
    char chunk2[] = "  13; }\n";
    const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};

    RunStreamingTest(chunks);
  }
}


TEST(StreamingUtf8Script) {
  // We'd want to write \uc481 instead of \xec\x92\x81, but Windows compilers
  // don't like it.
  const char* chunk1 =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob\xec\x92\x81r = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  const char* chunks[] = {chunk1, "foo(); ", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharactersSanityCheck) {
  // A sanity check to prove that the approach of splitting UTF-8
  // characters is correct. Here is an UTF-8 character which will take three
  // bytes.
  const char* reference = "\xec\x92\x81";
  CHECK_EQ(3, strlen(reference));

  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob";
  char chunk2[] =
      "XXXr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  for (int i = 0; i < 3; ++i) {
    chunk2[i] = reference[i];
  }
  const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharacters) {
  // Stream data where a multi-byte UTF-8 character is split between two data
  // chunks.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] =
      "XXr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharactersValidEdgeCases) {
  // Tests edge cases which should still be decoded correctly.

  // Case 1: a chunk contains only bytes for a split character (and no other
  // data). This kind of a chunk would be exceptionally small, but we should
  // still decode it correctly.
  const char* reference = "\xec\x92\x81";
  // The small chunk is at the beginning of the split character
  {
    char chunk1[] =
        "function foo() {\n"
        "  // This function will contain an UTF-8 character which is not in\n"
        "  // ASCII.\n"
        "  var foob";
    char chunk2[] = "XX";
    char chunk3[] =
        "Xr = 13;\n"
        "  return foob\xec\x92\x81r;\n"
        "}\n";
    chunk2[0] = reference[0];
    chunk2[1] = reference[1];
    chunk3[0] = reference[2];
    const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", nullptr};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
  // The small chunk is at the end of a character
  {
    char chunk1[] =
        "function foo() {\n"
        "  // This function will contain an UTF-8 character which is not in\n"
        "  // ASCII.\n"
        "  var foobX";
    char chunk2[] = "XX";
    char chunk3[] =
        "r = 13;\n"
        "  return foob\xec\x92\x81r;\n"
        "}\n";
    chunk1[strlen(chunk1) - 1] = reference[0];
    chunk2[0] = reference[1];
    chunk2[1] = reference[2];
    const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", nullptr};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
  // Case 2: the script ends with a multi-byte character. Make sure that it's
  // decoded correctly and not just ignored.
  {
    char chunk1[] =
        "var foob\xec\x92\x81 = 13;\n"
        "foob\xec\x92\x81";
    const char* chunks[] = {chunk1, nullptr};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
}


TEST(StreamingUtf8ScriptWithSplitCharactersInvalidEdgeCases) {
  // Test cases where a UTF-8 character is split over several chunks. Those
  // cases are not supported (the embedder should give the data in big enough
  // chunks), but we shouldn't crash and parse this just fine.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] = "X";
  char chunk3[] =
      "Xr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk3[0] = reference[2];
  const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", nullptr};

  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}



TEST(StreamingWithDebuggingEnabledLate) {
  // The streaming parser can only parse lazily, i.e. inner functions are not
  // fully parsed. However, we may compile inner functions eagerly when
  // debugging. Make sure that we can deal with this when turning on debugging
  // after streaming parser has already finished parsing.
  const char* chunks[] = {"with({x:1}) {",
                          "  var foo = function foo(y) {",
                          "    return x + y;",
                          "  };",
                          "  foo(2);",
                          "}",
                          nullptr};

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch(isolate);

  v8::ScriptCompiler::StreamedSource source(
      v8::base::make_unique<TestSourceStream>(chunks),
      v8::ScriptCompiler::StreamedSource::ONE_BYTE);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(isolate, &source);

  task->Run();
  delete task;

  CHECK(!try_catch.HasCaught());

  v8::ScriptOrigin origin(v8_str("http://foo.com"));
  char* full_source = TestSourceStream::FullSourceString(chunks);

  EnableDebugger(isolate);

  v8::Local<Script> script =
      v8::ScriptCompiler::Compile(env.local(), &source, v8_str(full_source),
                                  origin)
          .ToLocalChecked();

  Maybe<uint32_t> result =
      script->Run(env.local()).ToLocalChecked()->Uint32Value(env.local());
  CHECK_EQ(3U, result.FromMaybe(0));

  delete[] full_source;

  DisableDebugger(isolate);
}


TEST(StreamingScriptWithInvalidUtf8) {
  // Regression test for a crash: test that invalid UTF-8 bytes in the end of a
  // chunk don't produce a crash.
  const char* reference = "\xec\x92\x81\x80\x80";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobXXXXX";  // Too many bytes which look like incomplete chars!
  char chunk2[] =
      "r = 13;\n"
      "  return foob\xec\x92\x81\x80\x80r;\n"
      "}\n";
  for (int i = 0; i < 5; ++i) chunk1[strlen(chunk1) - 5 + i] = reference[i];

  const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, false);
}


TEST(StreamingUtf8ScriptWithMultipleMultibyteCharactersSomeSplit) {
  // Regression test: Stream data where there are several multi-byte UTF-8
  // characters in a sequence and one of them is split between two data chunks.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob\xec\x92\x81X";
  char chunk2[] =
      "XXr = 13;\n"
      "  return foob\xec\x92\x81\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithMultipleMultibyteCharactersSomeSplit2) {
  // Another regression test, similar to the previous one. The difference is
  // that the split character is not the last one in the sequence.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] =
      "XX\xec\x92\x81r = 13;\n"
      "  return foob\xec\x92\x81\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingWithHarmonyScopes) {
  // Don't use RunStreamingTest here so that both scripts get to use the same
  // LocalContext and HandleScope.
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // First, run a script with a let variable.
  CompileRun("\"use strict\"; let x = 1;");

  // Then stream a script which (erroneously) tries to introduce the same
  // variable again.
  const char* chunks[] = {"\"use strict\"; let x = 2;", nullptr};

  v8::TryCatch try_catch(isolate);
  v8::ScriptCompiler::StreamedSource source(
      v8::base::make_unique<TestSourceStream>(chunks),
      v8::ScriptCompiler::StreamedSource::ONE_BYTE);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(isolate, &source);
  task->Run();
  delete task;

  // Parsing should succeed (the script will be parsed and compiled in a context
  // independent way, so the error is not detected).
  CHECK(!try_catch.HasCaught());

  v8::ScriptOrigin origin(v8_str("http://foo.com"));
  char* full_source = TestSourceStream::FullSourceString(chunks);
  v8::Local<Script> script =
      v8::ScriptCompiler::Compile(env.local(), &source, v8_str(full_source),
                                  origin)
          .ToLocalChecked();
  CHECK(!script.IsEmpty());
  CHECK(!try_catch.HasCaught());

  // Running the script exposes the error.
  CHECK(script->Run(env.local()).IsEmpty());
  CHECK(try_catch.HasCaught());
  delete[] full_source;
}


TEST(CodeCache) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();

  const char* source = "Math.sqrt(4)";
  const char* origin = "code cache test";
  v8::ScriptCompiler::CachedData* cache;

  v8::Isolate* isolate1 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate1);
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope cscope(context);
    v8::Local<v8::String> source_string = v8_str(source);
    v8::ScriptOrigin script_origin(v8_str(origin));
    v8::ScriptCompiler::Source source(source_string, script_origin);
    v8::ScriptCompiler::CompileOptions option =
        v8::ScriptCompiler::kNoCompileOptions;
    v8::Local<v8::Script> script =
        v8::ScriptCompiler::Compile(context, &source, option).ToLocalChecked();
    cache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());
  }
  isolate1->Dispose();

  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope cscope(context);
    v8::Local<v8::String> source_string = v8_str(source);
    v8::ScriptOrigin script_origin(v8_str(origin));
    v8::ScriptCompiler::Source source(source_string, script_origin, cache);
    v8::ScriptCompiler::CompileOptions option =
        v8::ScriptCompiler::kConsumeCodeCache;
    v8::Local<v8::Script> script;
    {
      i::DisallowCompilation no_compile(
          reinterpret_cast<i::Isolate*>(isolate2));
      script = v8::ScriptCompiler::Compile(context, &source, option)
                   .ToLocalChecked();
    }
    CHECK_EQ(2, script->Run(context)
                    .ToLocalChecked()
                    ->ToInt32(context)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  isolate2->Dispose();
}

v8::MaybeLocal<Module> UnexpectedModuleResolveCallback(Local<Context> context,
                                                       Local<String> specifier,
                                                       Local<Module> referrer) {
  CHECK_WITH_MSG(false, "Unexpected call to resolve callback");
}

v8::MaybeLocal<Value> UnexpectedSyntheticModuleEvaluationStepsCallback(
    Local<Context> context, Local<Module> module) {
  CHECK_WITH_MSG(false, "Unexpected call to synthetic module re callback");
}

static int synthetic_module_callback_count;

v8::MaybeLocal<Value> SyntheticModuleEvaluationStepsCallback(
    Local<Context> context, Local<Module> module) {
  synthetic_module_callback_count++;
  return v8::Undefined(reinterpret_cast<v8::Isolate*>(context->GetIsolate()));
}

v8::MaybeLocal<Value> SyntheticModuleEvaluationStepsCallbackFail(
    Local<Context> context, Local<Module> module) {
  synthetic_module_callback_count++;
  context->GetIsolate()->ThrowException(
      v8_str("SyntheticModuleEvaluationStepsCallbackFail exception"));
  return v8::MaybeLocal<Value>();
}

v8::MaybeLocal<Value> SyntheticModuleEvaluationStepsCallbackSetExport(
    Local<Context> context, Local<Module> module) {
  Maybe<bool> set_export_result = module->SetSyntheticModuleExport(
      context->GetIsolate(), v8_str("test_export"), v8_num(42));
  CHECK(set_export_result.FromJust());
  return v8::Undefined(reinterpret_cast<v8::Isolate*>(context->GetIsolate()));
}

namespace {

Local<Module> CompileAndInstantiateModule(v8::Isolate* isolate,
                                          Local<Context> context,
                                          const char* resource_name,
                                          const char* source) {
  Local<String> source_string = v8_str(source);
  v8::ScriptOrigin script_origin(
      v8_str(resource_name), Local<v8::Integer>(), Local<v8::Integer>(),
      Local<v8::Boolean>(), Local<v8::Integer>(), Local<v8::Value>(),
      Local<v8::Boolean>(), Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source script_compiler_source(source_string,
                                                    script_origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &script_compiler_source)
          .ToLocalChecked();
  module->InstantiateModule(context, UnexpectedModuleResolveCallback)
      .ToChecked();

  return module;
}

Local<Module> CreateAndInstantiateSyntheticModule(
    v8::Isolate* isolate, Local<String> module_name, Local<Context> context,
    std::vector<v8::Local<v8::String>> export_names,
    v8::Module::SyntheticModuleEvaluationSteps evaluation_steps) {
  Local<Module> module = v8::Module::CreateSyntheticModule(
      isolate, module_name, export_names, evaluation_steps);
  module->InstantiateModule(context, UnexpectedModuleResolveCallback)
      .ToChecked();

  return module;
}

Local<Module> CompileAndInstantiateModuleFromCache(
    v8::Isolate* isolate, Local<Context> context, const char* resource_name,
    const char* source, v8::ScriptCompiler::CachedData* cache) {
  Local<String> source_string = v8_str(source);
  v8::ScriptOrigin script_origin(
      v8_str(resource_name), Local<v8::Integer>(), Local<v8::Integer>(),
      Local<v8::Boolean>(), Local<v8::Integer>(), Local<v8::Value>(),
      Local<v8::Boolean>(), Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source script_compiler_source(source_string,
                                                    script_origin, cache);

  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &script_compiler_source,
                                        v8::ScriptCompiler::kConsumeCodeCache)
          .ToLocalChecked();
  module->InstantiateModule(context, UnexpectedModuleResolveCallback)
      .ToChecked();

  return module;
}

}  // namespace

v8::MaybeLocal<Module> SyntheticModuleResolveCallback(Local<Context> context,
                                                      Local<String> specifier,
                                                      Local<Module> referrer) {
  std::vector<v8::Local<v8::String>> export_names{v8_str("test_export")};
  Local<Module> module = CreateAndInstantiateSyntheticModule(
      context->GetIsolate(),
      v8_str("SyntheticModuleResolveCallback-TestSyntheticModule"), context,
      export_names, SyntheticModuleEvaluationStepsCallbackSetExport);
  return v8::MaybeLocal<Module>(module);
}

v8::MaybeLocal<Module> SyntheticModuleThatThrowsDuringEvaluateResolveCallback(
    Local<Context> context, Local<String> specifier, Local<Module> referrer) {
  std::vector<v8::Local<v8::String>> export_names{v8_str("test_export")};
  Local<Module> module = CreateAndInstantiateSyntheticModule(
      context->GetIsolate(),
      v8_str("SyntheticModuleThatThrowsDuringEvaluateResolveCallback-"
             "TestSyntheticModule"),
      context, export_names, SyntheticModuleEvaluationStepsCallbackFail);
  return v8::MaybeLocal<Module>(module);
}

TEST(ModuleCodeCache) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();

  const char* origin = "code cache test";
  const char* source =
      "export default 5; export const a = 10; function f() { return 42; } "
      "(function() { return f(); })();";

  v8::ScriptCompiler::CachedData* cache;
  {
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);

      Local<Module> module =
          CompileAndInstantiateModule(isolate, context, origin, source);

      // Fetch the shared function info before evaluation.
      Local<v8::UnboundModuleScript> unbound_module_script =
          module->GetUnboundModuleScript();

      // Evaluate for possible lazy compilation.
      Local<Value> completion_value =
          module->Evaluate(context).ToLocalChecked();
      CHECK_EQ(42, completion_value->Int32Value(context).FromJust());

      // Now create the cache. Note that it is freed, obscurely, when
      // ScriptCompiler::Source goes out of scope below.
      cache = v8::ScriptCompiler::CreateCodeCache(unbound_module_script);
    }
    isolate->Dispose();
  }

  // Test that the cache is consumed and execution still works.
  {
    // Disable --always_opt, otherwise we try to optimize during module
    // instantiation, violating the DisallowCompilation scope.
    i::FLAG_always_opt = false;
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);

      Local<Module> module;
      {
        i::DisallowCompilation no_compile(
            reinterpret_cast<i::Isolate*>(isolate));
        module = CompileAndInstantiateModuleFromCache(isolate, context, origin,
                                                      source, cache);
      }

      Local<Value> completion_value =
          module->Evaluate(context).ToLocalChecked();
      CHECK_EQ(42, completion_value->Int32Value(context).FromJust());
    }
    isolate->Dispose();
  }
}

TEST(CreateSyntheticModule) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  auto i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  std::vector<v8::Local<v8::String>> export_names{v8_str("default")};

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate, v8_str("CreateSyntheticModule-TestSyntheticModule"), context,
      export_names, UnexpectedSyntheticModuleEvaluationStepsCallback);
  i::Handle<i::SyntheticModule> i_module =
      i::Handle<i::SyntheticModule>::cast(v8::Utils::OpenHandle(*module));
  i::Handle<i::ObjectHashTable> exports(i_module->exports(), i_isolate);
  i::Handle<i::String> default_name =
      i_isolate->factory()->NewStringFromAsciiChecked("default");

  CHECK(
      i::Handle<i::Object>(exports->Lookup(default_name), i_isolate)->IsCell());
  CHECK(i::Handle<i::Cell>::cast(
            i::Handle<i::Object>(exports->Lookup(default_name), i_isolate))
            ->value()
            .IsUndefined());
  CHECK_EQ(i_module->export_names().length(), 1);
  CHECK(i::String::cast(i_module->export_names().get(0)).Equals(*default_name));
  CHECK_EQ(i_module->status(), i::Module::kInstantiated);
}

TEST(SyntheticModuleSetExports) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  auto i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  Local<String> foo_string = v8_str("foo");
  Local<String> bar_string = v8_str("bar");
  std::vector<v8::Local<v8::String>> export_names{foo_string};

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate, v8_str("SyntheticModuleSetExports-TestSyntheticModule"), context,
      export_names, UnexpectedSyntheticModuleEvaluationStepsCallback);

  i::Handle<i::SyntheticModule> i_module =
      i::Handle<i::SyntheticModule>::cast(v8::Utils::OpenHandle(*module));
  i::Handle<i::ObjectHashTable> exports(i_module->exports(), i_isolate);

  i::Handle<i::Cell> foo_cell = i::Handle<i::Cell>::cast(i::Handle<i::Object>(
      exports->Lookup(v8::Utils::OpenHandle(*foo_string)), i_isolate));

  // During Instantiation there should be a Cell for the export initialized to
  // undefined.
  CHECK(foo_cell->value().IsUndefined());

  Maybe<bool> set_export_result =
      module->SetSyntheticModuleExport(isolate, foo_string, bar_string);
  CHECK(set_export_result.FromJust());

  // After setting the export the Cell should still have the same idenitity.
  CHECK_EQ(exports->Lookup(v8::Utils::OpenHandle(*foo_string)), *foo_cell);

  // Test that the export value was actually set.
  CHECK(i::Handle<i::String>::cast(
            i::Handle<i::Object>(foo_cell->value(), i_isolate))
            ->Equals(*v8::Utils::OpenHandle(*bar_string)));
}

TEST(SyntheticModuleSetMissingExport) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  auto i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  Local<String> foo_string = v8_str("foo");
  Local<String> bar_string = v8_str("bar");

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate, v8_str("SyntheticModuleSetExports-TestSyntheticModule"), context,
      std::vector<v8::Local<v8::String>>(),
      UnexpectedSyntheticModuleEvaluationStepsCallback);

  i::Handle<i::SyntheticModule> i_module =
      i::Handle<i::SyntheticModule>::cast(v8::Utils::OpenHandle(*module));
  i::Handle<i::ObjectHashTable> exports(i_module->exports(), i_isolate);

  TryCatch try_catch(isolate);
  Maybe<bool> set_export_result =
      module->SetSyntheticModuleExport(isolate, foo_string, bar_string);
  CHECK(set_export_result.IsNothing());
  CHECK(try_catch.HasCaught());
}

TEST(SyntheticModuleEvaluationStepsNoThrow) {
  synthetic_module_callback_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  std::vector<v8::Local<v8::String>> export_names{v8_str("default")};

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate,
      v8_str("SyntheticModuleEvaluationStepsNoThrow-TestSyntheticModule"),
      context, export_names, SyntheticModuleEvaluationStepsCallback);
  CHECK_EQ(synthetic_module_callback_count, 0);
  Local<Value> completion_value = module->Evaluate(context).ToLocalChecked();
  CHECK(completion_value->IsUndefined());
  CHECK_EQ(synthetic_module_callback_count, 1);
  CHECK_EQ(module->GetStatus(), Module::kEvaluated);
}

TEST(SyntheticModuleEvaluationStepsThrow) {
  synthetic_module_callback_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  v8::Context::Scope cscope(context);

  std::vector<v8::Local<v8::String>> export_names{v8_str("default")};

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate,
      v8_str("SyntheticModuleEvaluationStepsThrow-TestSyntheticModule"),
      context, export_names, SyntheticModuleEvaluationStepsCallbackFail);
  TryCatch try_catch(isolate);
  CHECK_EQ(synthetic_module_callback_count, 0);
  v8::MaybeLocal<Value> completion_value = module->Evaluate(context);
  CHECK(completion_value.IsEmpty());
  CHECK_EQ(synthetic_module_callback_count, 1);
  CHECK_EQ(module->GetStatus(), Module::kErrored);
  CHECK(try_catch.HasCaught());
}

TEST(SyntheticModuleEvaluationStepsSetExport) {
  synthetic_module_callback_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  auto i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  Local<String> test_export_string = v8_str("test_export");
  std::vector<v8::Local<v8::String>> export_names{test_export_string};

  Local<Module> module = CreateAndInstantiateSyntheticModule(
      isolate,
      v8_str("SyntheticModuleEvaluationStepsSetExport-TestSyntheticModule"),
      context, export_names, SyntheticModuleEvaluationStepsCallbackSetExport);

  i::Handle<i::SyntheticModule> i_module =
      i::Handle<i::SyntheticModule>::cast(v8::Utils::OpenHandle(*module));
  i::Handle<i::ObjectHashTable> exports(i_module->exports(), i_isolate);

  i::Handle<i::Cell> test_export_cell =
      i::Handle<i::Cell>::cast(i::Handle<i::Object>(
          exports->Lookup(v8::Utils::OpenHandle(*test_export_string)),
          i_isolate));
  CHECK(test_export_cell->value().IsUndefined());

  Local<Value> completion_value = module->Evaluate(context).ToLocalChecked();
  CHECK(completion_value->IsUndefined());
  CHECK_EQ(42, test_export_cell->value().Number());
  CHECK_EQ(module->GetStatus(), Module::kEvaluated);
}

TEST(ImportFromSyntheticModule) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  Local<String> url = v8_str("www.test.com");
  Local<String> source_text = v8_str(
      "import {test_export} from './synthetic.module';"
      "(function() { return test_export; })();");
  v8::ScriptOrigin origin(url, Local<v8::Integer>(), Local<v8::Integer>(),
                          Local<v8::Boolean>(), Local<v8::Integer>(),
                          Local<v8::Value>(), Local<v8::Boolean>(),
                          Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(source_text, origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
  module->InstantiateModule(context, SyntheticModuleResolveCallback)
      .ToChecked();

  Local<Value> completion_value = module->Evaluate(context).ToLocalChecked();
  CHECK_EQ(42, completion_value->Int32Value(context).FromJust());
}

TEST(ImportFromSyntheticModuleThrow) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Isolate::Scope iscope(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  v8::Context::Scope cscope(context);

  Local<String> url = v8_str("www.test.com");
  Local<String> source_text = v8_str(
      "import {test_export} from './synthetic.module';"
      "(function() { return test_export; })();");
  v8::ScriptOrigin origin(url, Local<v8::Integer>(), Local<v8::Integer>(),
                          Local<v8::Boolean>(), Local<v8::Integer>(),
                          Local<v8::Value>(), Local<v8::Boolean>(),
                          Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(source_text, origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
  module
      ->InstantiateModule(
          context, SyntheticModuleThatThrowsDuringEvaluateResolveCallback)
      .ToChecked();

  CHECK_EQ(module->GetStatus(), Module::kInstantiated);
  TryCatch try_catch(isolate);
  v8::MaybeLocal<Value> completion_value = module->Evaluate(context);
  CHECK(completion_value.IsEmpty());
  CHECK_EQ(module->GetStatus(), Module::kErrored);
  CHECK(try_catch.HasCaught());
}

// Tests that the code cache does not confuse the same source code compiled as a
// script and as a module.
TEST(CodeCacheModuleScriptMismatch) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();

  const char* origin = "code cache test";
  const char* source = "42";

  v8::ScriptCompiler::CachedData* cache;
  {
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);

      Local<Module> module =
          CompileAndInstantiateModule(isolate, context, origin, source);

      // Fetch the shared function info before evaluation.
      Local<v8::UnboundModuleScript> unbound_module_script =
          module->GetUnboundModuleScript();

      // Evaluate for possible lazy compilation.
      Local<Value> completion_value =
          module->Evaluate(context).ToLocalChecked();
      CHECK_EQ(42, completion_value->Int32Value(context).FromJust());

      // Now create the cache. Note that it is freed, obscurely, when
      // ScriptCompiler::Source goes out of scope below.
      cache = v8::ScriptCompiler::CreateCodeCache(unbound_module_script);
    }
    isolate->Dispose();
  }

  // Test that the cache is not consumed when source is compiled as a script.
  {
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);

      v8::ScriptOrigin script_origin(v8_str(origin));
      v8::ScriptCompiler::Source script_compiler_source(v8_str(source),
                                                        script_origin, cache);

      v8::Local<v8::Script> script =
          v8::ScriptCompiler::Compile(context, &script_compiler_source,
                                      v8::ScriptCompiler::kConsumeCodeCache)
              .ToLocalChecked();

      CHECK(cache->rejected);

      CHECK_EQ(42, script->Run(context)
                       .ToLocalChecked()
                       ->ToInt32(context)
                       .ToLocalChecked()
                       ->Int32Value(context)
                       .FromJust());
    }
    isolate->Dispose();
  }
}

// Same as above but other way around.
TEST(CodeCacheScriptModuleMismatch) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();

  const char* origin = "code cache test";
  const char* source = "42";

  v8::ScriptCompiler::CachedData* cache;
  {
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);
      v8::Local<v8::String> source_string = v8_str(source);
      v8::ScriptOrigin script_origin(v8_str(origin));
      v8::ScriptCompiler::Source source(source_string, script_origin);
      v8::ScriptCompiler::CompileOptions option =
          v8::ScriptCompiler::kNoCompileOptions;
      v8::Local<v8::Script> script =
          v8::ScriptCompiler::Compile(context, &source, option)
              .ToLocalChecked();
      cache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());
    }
    isolate->Dispose();
  }

  // Test that the cache is not consumed when source is compiled as a module.
  {
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
      v8::Isolate::Scope iscope(isolate);
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope cscope(context);

      v8::ScriptOrigin script_origin(
          v8_str(origin), Local<v8::Integer>(), Local<v8::Integer>(),
          Local<v8::Boolean>(), Local<v8::Integer>(), Local<v8::Value>(),
          Local<v8::Boolean>(), Local<v8::Boolean>(), True(isolate));
      v8::ScriptCompiler::Source script_compiler_source(v8_str(source),
                                                        script_origin, cache);

      Local<Module> module = v8::ScriptCompiler::CompileModule(
                                 isolate, &script_compiler_source,
                                 v8::ScriptCompiler::kConsumeCodeCache)
                                 .ToLocalChecked();
      module->InstantiateModule(context, UnexpectedModuleResolveCallback)
          .ToChecked();

      CHECK(cache->rejected);

      Local<Value> completion_value =
          module->Evaluate(context).ToLocalChecked();
      CHECK_EQ(42, completion_value->Int32Value(context).FromJust());
    }
    isolate->Dispose();
  }
}

// Tests that compilation can handle a garbled cache.
TEST(InvalidCodeCacheDataInCompileModule) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext local_context;

  const char* garbage = "garbage garbage garbage garbage garbage garbage";
  const uint8_t* data = reinterpret_cast<const uint8_t*>(garbage);
  Local<String> origin = v8_str("origin");
  int length = 16;
  v8::ScriptCompiler::CachedData* cached_data =
      new v8::ScriptCompiler::CachedData(data, length);
  CHECK(!cached_data->rejected);

  v8::ScriptOrigin script_origin(
      origin, Local<v8::Integer>(), Local<v8::Integer>(), Local<v8::Boolean>(),
      Local<v8::Integer>(), Local<v8::Value>(), Local<v8::Boolean>(),
      Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(v8_str("42"), script_origin, cached_data);
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();

  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source,
                                        v8::ScriptCompiler::kConsumeCodeCache)
          .ToLocalChecked();
  module->InstantiateModule(context, UnexpectedModuleResolveCallback)
      .ToChecked();

  CHECK(cached_data->rejected);
  CHECK_EQ(42, module->Evaluate(context)
                   .ToLocalChecked()
                   ->Int32Value(context)
                   .FromJust());
}

void TestInvalidCacheData(v8::ScriptCompiler::CompileOptions option) {
  const char* garbage = "garbage garbage garbage garbage garbage garbage";
  const uint8_t* data = reinterpret_cast<const uint8_t*>(garbage);
  int length = 16;
  v8::ScriptCompiler::CachedData* cached_data =
      new v8::ScriptCompiler::CachedData(data, length);
  CHECK(!cached_data->rejected);
  v8::ScriptOrigin origin(v8_str("origin"));
  v8::ScriptCompiler::Source source(v8_str("42"), origin, cached_data);
  v8::Local<v8::Context> context = CcTest::isolate()->GetCurrentContext();
  v8::Local<v8::Script> script =
      v8::ScriptCompiler::Compile(context, &source, option).ToLocalChecked();
  CHECK(cached_data->rejected);
  CHECK_EQ(
      42,
      script->Run(context).ToLocalChecked()->Int32Value(context).FromJust());
}


TEST(InvalidCodeCacheData) {
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  LocalContext context;
  TestInvalidCacheData(v8::ScriptCompiler::kConsumeCodeCache);
}


TEST(StringConcatOverflow) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  RandomLengthOneByteResource* r =
      new RandomLengthOneByteResource(i::String::kMaxLength);
  v8::Local<v8::String> str =
      v8::String::NewExternalOneByte(isolate, r).ToLocalChecked();
  CHECK(!str.IsEmpty());
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> result = v8::String::Concat(isolate, str, str);
  v8::String::Concat(CcTest::isolate(), str, str);
  CHECK(result.IsEmpty());
  CHECK(!try_catch.HasCaught());
}

TEST(TurboAsmDisablesDetach) {
#ifndef V8_LITE_MODE
  i::FLAG_opt = true;
  i::FLAG_allow_natives_syntax = true;
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  LocalContext context;
  const char* load =
      "function Module(stdlib, foreign, heap) {"
      "  'use asm';"
      "  var MEM32 = new stdlib.Int32Array(heap);"
      "  function load() { return MEM32[0] | 0; }"
      "  return { load: load };"
      "}"
      "var buffer = new ArrayBuffer(4096);"
      "var module = Module(this, {}, buffer);"
      "%PrepareFunctionForOptimization(module.load);"
      "%OptimizeFunctionOnNextCall(module.load);"
      "module.load();"
      "buffer";

  v8::Local<v8::ArrayBuffer> result = CompileRun(load).As<v8::ArrayBuffer>();
  CHECK(!result->IsDetachable());

  const char* store =
      "function Module(stdlib, foreign, heap) {"
      "  'use asm';"
      "  var MEM32 = new stdlib.Int32Array(heap);"
      "  function store() { MEM32[0] = 0; }"
      "  return { store: store };"
      "}"
      "var buffer = new ArrayBuffer(4096);"
      "var module = Module(this, {}, buffer);"
      "%PrepareFunctionForOptimization(module.store);"
      "%OptimizeFunctionOnNextCall(module.store);"
      "module.store();"
      "buffer";

  result = CompileRun(store).As<v8::ArrayBuffer>();
  CHECK(!result->IsDetachable());
#endif  // V8_LITE_MODE
}

TEST(ClassPrototypeCreationContext) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  Local<Object> result = Local<Object>::Cast(
      CompileRun("'use strict'; class Example { }; Example.prototype"));
  CHECK(env.local() == result->CreationContext());
}


TEST(SimpleStreamingScriptWithSourceURL) {
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo();\n",
                          "//# sourceURL=bar2.js\n", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true,
                   "bar2.js");
}


TEST(StreamingScriptWithSplitSourceURL) {
  const char* chunks[] = {"function foo() { ret", "urn 13; } f",
                          "oo();\n//# sourceURL=b", "ar2.js\n", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true,
                   "bar2.js");
}


TEST(StreamingScriptWithSourceMappingURLInTheMiddle) {
  const char* chunks[] = {"function foo() { ret", "urn 13; }\n//#",
                          " sourceMappingURL=bar2.js\n", "foo();", nullptr};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true,
                   nullptr, "bar2.js");
}


TEST(NewStringRangeError) {
  // This test uses a lot of memory and fails with flaky OOM when run
  // with --stress-incremental-marking on TSAN.
  i::FLAG_stress_incremental_marking = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  const int length = i::String::kMaxLength + 1;
  const int buffer_size = length * sizeof(uint16_t);
  void* buffer = malloc(buffer_size);
  if (buffer == nullptr) return;
  memset(buffer, 'A', buffer_size);
  {
    v8::TryCatch try_catch(isolate);
    char* data = reinterpret_cast<char*>(buffer);
    CHECK(v8::String::NewFromUtf8(isolate, data, v8::NewStringType::kNormal,
                                  length)
              .IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  {
    v8::TryCatch try_catch(isolate);
    uint8_t* data = reinterpret_cast<uint8_t*>(buffer);
    CHECK(v8::String::NewFromOneByte(isolate, data, v8::NewStringType::kNormal,
                                     length)
              .IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  {
    v8::TryCatch try_catch(isolate);
    uint16_t* data = reinterpret_cast<uint16_t*>(buffer);
    CHECK(v8::String::NewFromTwoByte(isolate, data, v8::NewStringType::kNormal,
                                     length)
              .IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  free(buffer);
}


TEST(SealHandleScope) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::SealHandleScope seal(isolate);

  // Should fail
  v8::Local<v8::Object> obj = v8::Object::New(isolate);

  USE(obj);
}


TEST(SealHandleScopeNested) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::SealHandleScope seal(isolate);

  {
    v8::HandleScope handle_scope(isolate);

    // Should work
    v8::Local<v8::Object> obj = v8::Object::New(isolate);

    USE(obj);
  }
}


static void ExtrasBindingTestRuntimeFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK_EQ(
      3,
      args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromJust());
  args.GetReturnValue().Set(v8_num(7));
}

TEST(ExtrasFunctionSource) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  // Functions defined in extras do not expose source code.
  auto func = binding->Get(env.local(), v8_str("testFunctionToString"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  auto undefined = v8::Undefined(isolate);
  auto result = func->Call(env.local(), undefined, 0, {})
                    .ToLocalChecked()
                    .As<v8::String>();
  CHECK(result->StrictEquals(v8_str("function foo() { [native code] }")));

  // Functions defined in extras do not show up in the stack trace.
  auto wrapper = binding->Get(env.local(), v8_str("testStackTrace"))
                     .ToLocalChecked()
                     .As<v8::Function>();
  CHECK(env->Global()->Set(env.local(), v8_str("wrapper"), wrapper).FromJust());
  ExpectString(
      "function f(x) { return wrapper(x) }"
      "function g() { return new Error().stack; }"
      "f(g)",
      "Error\n"
      "    at g (<anonymous>:1:58)\n"
      "    at f (<anonymous>:1:24)\n"
      "    at <anonymous>:1:78");
}

TEST(ExtrasBindingObject) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  // standalone.gypi ensures we include the test-extra.js file, which should
  // export the tested functions.
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testExtraShouldReturnFive"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  auto undefined = v8::Undefined(isolate);
  auto result = func->Call(env.local(), undefined, 0, {})
                    .ToLocalChecked()
                    .As<v8::Number>();
  CHECK_EQ(5, result->Int32Value(env.local()).FromJust());

  v8::Local<v8::FunctionTemplate> runtimeFunction =
      v8::FunctionTemplate::New(isolate, ExtrasBindingTestRuntimeFunction);
  binding->Set(env.local(), v8_str("runtime"),
               runtimeFunction->GetFunction(env.local()).ToLocalChecked())
      .FromJust();
  func = binding->Get(env.local(), v8_str("testExtraShouldCallToRuntime"))
             .ToLocalChecked()
             .As<v8::Function>();
  result = func->Call(env.local(), undefined, 0, {})
               .ToLocalChecked()
               .As<v8::Number>();
  CHECK_EQ(7, result->Int32Value(env.local()).FromJust());
}


TEST(ExtrasCreatePromise) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  LocalContext env;
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testCreatePromise"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());

  auto promise = CompileRun(
                     "%PrepareFunctionForOptimization(func);\n"
                     "func();\n"
                     "func();\n"
                     "%OptimizeFunctionOnNextCall(func);\n"
                     "func()\n")
                     .As<v8::Promise>();
  CHECK_EQ(v8::Promise::kPending, promise->State());
}

TEST(ExtrasCreatePromiseWithParent) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  LocalContext env;
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testCreatePromiseWithParent"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());

  auto promise = CompileRun(
                     "var parent = new Promise((a, b) => {});\n"
                     "%PrepareFunctionForOptimization(func);\n"
                     "func(parent);\n"
                     "func(parent);\n"
                     "%OptimizeFunctionOnNextCall(func);\n"
                     "func(parent)\n")
                     .As<v8::Promise>();
  CHECK_EQ(v8::Promise::kPending, promise->State());
}

TEST(ExtrasRejectPromise) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  LocalContext env;
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testRejectPromise"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());

  auto rejected_promise = CompileRun(
                              "function newPromise() {\n"
                              "  return new Promise((a, b) => {});\n"
                              "}\n"
                              "%PrepareFunctionForOptimization(func);\n"
                              "func(newPromise(), 1);\n"
                              "func(newPromise(), 1);\n"
                              "%OptimizeFunctionOnNextCall(func);\n"
                              "var promise = newPromise();\n"
                              "func(promise, 1);\n"
                              "promise;\n")
                              .As<v8::Promise>();
  CHECK_EQ(v8::Promise::kRejected, rejected_promise->State());
  CHECK_EQ(1, rejected_promise->Result()->Int32Value(env.local()).FromJust());
}

TEST(ExtrasResolvePromise) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  LocalContext env;
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testResolvePromise"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());

  auto pending_promise = CompileRun(
                             "function newPromise() {\n"
                             "  return new Promise((a, b) => {});\n"
                             "}\n"
                             "%PrepareFunctionForOptimization(func);\n"
                             "func(newPromise(), newPromise());\n"
                             "func(newPromise(), newPromise());\n"
                             "%OptimizeFunctionOnNextCall(func);\n"
                             "var promise = newPromise();\n"
                             "func(promise, newPromise());\n"
                             "promise;\n")
                             .As<v8::Promise>();
  CHECK_EQ(v8::Promise::kPending, pending_promise->State());

  auto fulfilled_promise = CompileRun(
                               "function newPromise() {\n"
                               "  return new Promise((a, b) => {});\n"
                               "}\n"
                               "%PrepareFunctionForOptimization(func);\n"
                               "func(newPromise(), 1);\n"
                               "func(newPromise(), 1);\n"
                               "%OptimizeFunctionOnNextCall(func);\n"
                               "var promise = newPromise();\n"
                               "func(promise, 1);\n"
                               "promise;\n")
                               .As<v8::Promise>();
  CHECK_EQ(v8::Promise::kFulfilled, fulfilled_promise->State());
  CHECK_EQ(1, fulfilled_promise->Result()->Int32Value(env.local()).FromJust());
}

TEST(ExtrasUtilsObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  LocalContext env;
  v8::Local<v8::Object> binding = env->GetExtrasBindingObject();

  auto func = binding->Get(env.local(), v8_str("testExtraCanUseUtils"))
                  .ToLocalChecked()
                  .As<v8::Function>();
  auto undefined = v8::Undefined(isolate);
  auto result = func->Call(env.local(), undefined, 0, {})
                    .ToLocalChecked()
                    .As<v8::Object>();

  auto private_symbol = result->Get(env.local(), v8_str("privateSymbol"))
                            .ToLocalChecked()
                            .As<v8::Symbol>();
  i::Handle<i::Symbol> ips = v8::Utils::OpenHandle(*private_symbol);
  CHECK(ips->IsPrivate());

  CompileRun("var result = 0; function store(x) { result = x; }");
  auto store = CompileRun("store").As<v8::Function>();

  auto fulfilled_promise = result->Get(env.local(), v8_str("fulfilledPromise"))
                               .ToLocalChecked()
                               .As<v8::Promise>();
  fulfilled_promise->Then(env.local(), store).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(1, CompileRun("result")->Int32Value(env.local()).FromJust());

  auto fulfilled_promise_2 =
      result->Get(env.local(), v8_str("fulfilledPromise2"))
          .ToLocalChecked()
          .As<v8::Promise>();
  fulfilled_promise_2->Then(env.local(), store).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(2, CompileRun("result")->Int32Value(env.local()).FromJust());

  auto rejected_promise = result->Get(env.local(), v8_str("rejectedPromise"))
                              .ToLocalChecked()
                              .As<v8::Promise>();
  rejected_promise->Catch(env.local(), store).ToLocalChecked();
  isolate->RunMicrotasks();
  CHECK_EQ(3, CompileRun("result")->Int32Value(env.local()).FromJust());

  auto rejected_but_handled_promise =
      result->Get(env.local(), v8_str("rejectedButHandledPromise"))
          .ToLocalChecked()
          .As<v8::Promise>();
  CHECK(rejected_but_handled_promise->HasHandler());

  auto promise_states = result->Get(env.local(), v8_str("promiseStates"))
                            .ToLocalChecked()
                            .As<v8::String>();
  String::Utf8Value promise_states_string(isolate, promise_states);
  CHECK_EQ(0, strcmp(*promise_states_string, "pending fulfilled rejected"));

  auto promise_is_promise = result->Get(env.local(), v8_str("promiseIsPromise"))
                                .ToLocalChecked()
                                .As<v8::Boolean>();
  CHECK_EQ(true, promise_is_promise->Value());

  auto thenable_is_promise =
      result->Get(env.local(), v8_str("thenableIsPromise"))
          .ToLocalChecked()
          .As<v8::Boolean>();
  CHECK_EQ(false, thenable_is_promise->Value());

  auto uncurry_this = result->Get(env.local(), v8_str("uncurryThis"))
                          .ToLocalChecked()
                          .As<v8::Boolean>();
  CHECK_EQ(true, uncurry_this->Value());
}


TEST(Map) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::Local<v8::Map> map = v8::Map::New(isolate);
  CHECK(map->IsObject());
  CHECK(map->IsMap());
  CHECK(map->GetPrototype()->StrictEquals(CompileRun("Map.prototype")));
  CHECK_EQ(0U, map->Size());

  v8::Local<v8::Value> val = CompileRun("new Map([[1, 2], [3, 4]])");
  CHECK(val->IsMap());
  map = v8::Local<v8::Map>::Cast(val);
  CHECK_EQ(2U, map->Size());

  v8::Local<v8::Array> contents = map->AsArray();
  CHECK_EQ(4U, contents->Length());
  CHECK_EQ(
      1,
      contents->Get(env.local(), 0).ToLocalChecked().As<v8::Int32>()->Value());
  CHECK_EQ(
      2,
      contents->Get(env.local(), 1).ToLocalChecked().As<v8::Int32>()->Value());
  CHECK_EQ(
      3,
      contents->Get(env.local(), 2).ToLocalChecked().As<v8::Int32>()->Value());
  CHECK_EQ(
      4,
      contents->Get(env.local(), 3).ToLocalChecked().As<v8::Int32>()->Value());

  CHECK_EQ(2U, map->Size());

  CHECK(map->Has(env.local(), v8::Integer::New(isolate, 1)).FromJust());
  CHECK(map->Has(env.local(), v8::Integer::New(isolate, 3)).FromJust());

  CHECK(!map->Has(env.local(), v8::Integer::New(isolate, 2)).FromJust());
  CHECK(!map->Has(env.local(), map).FromJust());

  CHECK_EQ(2, map->Get(env.local(), v8::Integer::New(isolate, 1))
                  .ToLocalChecked()
                  ->Int32Value(env.local())
                  .FromJust());
  CHECK_EQ(4, map->Get(env.local(), v8::Integer::New(isolate, 3))
                  .ToLocalChecked()
                  ->Int32Value(env.local())
                  .FromJust());

  CHECK(map->Get(env.local(), v8::Integer::New(isolate, 42))
            .ToLocalChecked()
            ->IsUndefined());

  CHECK(!map->Set(env.local(), map, map).IsEmpty());
  CHECK_EQ(3U, map->Size());
  CHECK(map->Has(env.local(), map).FromJust());

  CHECK(map->Delete(env.local(), map).FromJust());
  CHECK_EQ(2U, map->Size());
  CHECK(!map->Has(env.local(), map).FromJust());
  CHECK(!map->Delete(env.local(), map).FromJust());

  map->Clear();
  CHECK_EQ(0U, map->Size());
}


TEST(Set) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::Local<v8::Set> set = v8::Set::New(isolate);
  CHECK(set->IsObject());
  CHECK(set->IsSet());
  CHECK(set->GetPrototype()->StrictEquals(CompileRun("Set.prototype")));
  CHECK_EQ(0U, set->Size());

  v8::Local<v8::Value> val = CompileRun("new Set([1, 2])");
  CHECK(val->IsSet());
  set = v8::Local<v8::Set>::Cast(val);
  CHECK_EQ(2U, set->Size());

  v8::Local<v8::Array> keys = set->AsArray();
  CHECK_EQ(2U, keys->Length());
  CHECK_EQ(1,
           keys->Get(env.local(), 0).ToLocalChecked().As<v8::Int32>()->Value());
  CHECK_EQ(2,
           keys->Get(env.local(), 1).ToLocalChecked().As<v8::Int32>()->Value());

  CHECK_EQ(2U, set->Size());

  CHECK(set->Has(env.local(), v8::Integer::New(isolate, 1)).FromJust());
  CHECK(set->Has(env.local(), v8::Integer::New(isolate, 2)).FromJust());

  CHECK(!set->Has(env.local(), v8::Integer::New(isolate, 3)).FromJust());
  CHECK(!set->Has(env.local(), set).FromJust());

  CHECK(!set->Add(env.local(), set).IsEmpty());
  CHECK_EQ(3U, set->Size());
  CHECK(set->Has(env.local(), set).FromJust());

  CHECK(set->Delete(env.local(), set).FromJust());
  CHECK_EQ(2U, set->Size());
  CHECK(!set->Has(env.local(), set).FromJust());
  CHECK(!set->Delete(env.local(), set).FromJust());

  set->Clear();
  CHECK_EQ(0U, set->Size());
}

TEST(SetDeleteThenAsArray) {
  // https://bugs.chromium.org/p/v8/issues/detail?id=4946
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  // make a Set
  v8::Local<v8::Value> val = CompileRun("new Set([1, 2, 3])");
  v8::Local<v8::Set> set = v8::Local<v8::Set>::Cast(val);
  CHECK_EQ(3U, set->Size());

  // delete the "middle" element (using AsArray to
  // determine which element is the "middle" element)
  v8::Local<v8::Array> array1 = set->AsArray();
  CHECK_EQ(3U, array1->Length());
  CHECK(set->Delete(env.local(), array1->Get(env.local(), 1).ToLocalChecked())
            .FromJust());

  // make sure there are no undefined values when we convert to an array again.
  v8::Local<v8::Array> array2 = set->AsArray();
  uint32_t length = array2->Length();
  CHECK_EQ(2U, length);
  for (uint32_t i = 0; i < length; i++) {
    CHECK(!array2->Get(env.local(), i).ToLocalChecked()->IsUndefined());
  }
}

TEST(MapDeleteThenAsArray) {
  // https://bugs.chromium.org/p/v8/issues/detail?id=4946
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  // make a Map
  v8::Local<v8::Value> val = CompileRun("new Map([[1, 2], [3, 4], [5, 6]])");
  v8::Local<v8::Map> map = v8::Local<v8::Map>::Cast(val);
  CHECK_EQ(3U, map->Size());

  // delete the "middle" element (using AsArray to
  // determine which element is the "middle" element)
  v8::Local<v8::Array> array1 = map->AsArray();
  CHECK_EQ(6U, array1->Length());
  // Map::AsArray returns a flat array, so the second key is at index 2.
  v8::Local<v8::Value> key = array1->Get(env.local(), 2).ToLocalChecked();
  CHECK(map->Delete(env.local(), key).FromJust());

  // make sure there are no undefined values when we convert to an array again.
  v8::Local<v8::Array> array2 = map->AsArray();
  uint32_t length = array2->Length();
  CHECK_EQ(4U, length);
  for (uint32_t i = 0; i < length; i++) {
    CHECK(!array2->Get(env.local(), i).ToLocalChecked()->IsUndefined());
  }
}

TEST(CompatibleReceiverCheckOnCachedICHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> parent = FunctionTemplate::New(isolate);
  v8::Local<v8::Signature> signature = v8::Signature::New(isolate, parent);
  auto returns_42 =
      v8::FunctionTemplate::New(isolate, Returns42, Local<Value>(), signature);
  parent->PrototypeTemplate()->SetAccessorProperty(v8_str("age"), returns_42);
  v8::Local<v8::FunctionTemplate> child = v8::FunctionTemplate::New(isolate);
  child->Inherit(parent);
  LocalContext env;
  CHECK(env->Global()
            ->Set(env.local(), v8_str("Child"),
                  child->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  // Make sure there's a compiled stub for "Child.prototype.age" in the cache.
  CompileRun(
      "var real = new Child();\n"
      "for (var i = 0; i < 3; ++i) {\n"
      "  real.age;\n"
      "}\n");

  // Check that the cached stub is never used.
  ExpectInt32(
      "var fake = Object.create(Child.prototype);\n"
      "var result = 0;\n"
      "function test(d) {\n"
      "  if (d == 3) return;\n"
      "  try {\n"
      "    fake.age;\n"
      "    result = 1;\n"
      "  } catch (e) {\n"
      "  }\n"
      "  test(d+1);\n"
      "}\n"
      "test(0);\n"
      "result;\n",
      0);
}

THREADED_TEST(ReceiverConversionForAccessors) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> acc =
      v8::FunctionTemplate::New(isolate, Returns42);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("acc"),
                  acc->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessorProperty(v8_str("acc"), acc, acc);
  Local<v8::Object> instance = templ->NewInstance(env.local()).ToLocalChecked();

  CHECK(env->Global()->Set(env.local(), v8_str("p"), instance).FromJust());
  CHECK(CompileRun("(p.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(p.acc = 7) == 7")->BooleanValue(isolate));

  CHECK(!CompileRun("Number.prototype.__proto__ = p;"
                    "var a = 1;")
             .IsEmpty());
  CHECK(CompileRun("(a.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(a.acc = 7) == 7")->BooleanValue(isolate));

  CHECK(!CompileRun("Boolean.prototype.__proto__ = p;"
                    "var a = true;")
             .IsEmpty());
  CHECK(CompileRun("(a.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(a.acc = 7) == 7")->BooleanValue(isolate));

  CHECK(!CompileRun("String.prototype.__proto__ = p;"
                    "var a = 'foo';")
             .IsEmpty());
  CHECK(CompileRun("(a.acc == 42)")->BooleanValue(isolate));
  CHECK(CompileRun("(a.acc = 7) == 7")->BooleanValue(isolate));

  CHECK(CompileRun("acc.call(1) == 42")->BooleanValue(isolate));
  CHECK(CompileRun("acc.call(true)==42")->BooleanValue(isolate));
  CHECK(CompileRun("acc.call('aa')==42")->BooleanValue(isolate));
  CHECK(CompileRun("acc.call(null) == 42")->BooleanValue(isolate));
  CHECK(CompileRun("acc.call(undefined) == 42")->BooleanValue(isolate));
}

class FutexInterruptionThread : public v8::base::Thread {
 public:
  explicit FutexInterruptionThread(v8::Isolate* isolate)
      : Thread(Options("FutexInterruptionThread")), isolate_(isolate) {}

  void Run() override {
    // Wait a bit before terminating.
    v8::base::OS::Sleep(v8::base::TimeDelta::FromMilliseconds(100));
    isolate_->TerminateExecution();
  }

 private:
  v8::Isolate* isolate_;
};


TEST(FutexInterruption) {
  i::FLAG_harmony_sharedarraybuffer = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  FutexInterruptionThread timeout_thread(isolate);

  v8::TryCatch try_catch(CcTest::isolate());
  CHECK(timeout_thread.Start());

  CompileRun(
      "var ab = new SharedArrayBuffer(4);"
      "var i32a = new Int32Array(ab);"
      "Atomics.wait(i32a, 0, 0);");
  CHECK(try_catch.HasTerminated());
  timeout_thread.Join();
}

static int nb_uncaught_exception_callback_calls = 0;


bool NoAbortOnUncaughtException(v8::Isolate* isolate) {
  ++nb_uncaught_exception_callback_calls;
  return false;
}


TEST(AbortOnUncaughtExceptionNoAbort) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  LocalContext env(nullptr, global_template);

  i::FLAG_abort_on_uncaught_exception = true;
  isolate->SetAbortOnUncaughtExceptionCallback(NoAbortOnUncaughtException);

  CompileRun("function boom() { throw new Error(\"boom\") }");

  v8::Local<v8::Object> global_object = env->Global();
  v8::Local<v8::Function> foo = v8::Local<v8::Function>::Cast(
      global_object->Get(env.local(), v8_str("boom")).ToLocalChecked());

  CHECK(foo->Call(env.local(), global_object, 0, nullptr).IsEmpty());

  CHECK_EQ(1, nb_uncaught_exception_callback_calls);
}


TEST(AccessCheckedIsConcatSpreadable) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);
  LocalContext env;

  // Object with access check
  Local<ObjectTemplate> spreadable_template = v8::ObjectTemplate::New(isolate);
  spreadable_template->SetAccessCheckCallback(AccessBlocker);
  spreadable_template->Set(v8::Symbol::GetIsConcatSpreadable(isolate),
                           v8::Boolean::New(isolate, true));
  Local<Object> object =
      spreadable_template->NewInstance(env.local()).ToLocalChecked();

  allowed_access = true;
  CHECK(env->Global()->Set(env.local(), v8_str("object"), object).FromJust());
  object->Set(env.local(), v8_str("length"), v8_num(2)).FromJust();
  object->Set(env.local(), 0U, v8_str("a")).FromJust();
  object->Set(env.local(), 1U, v8_str("b")).FromJust();

  // Access check is allowed, and the object is spread
  CompileRun("var result = [].concat(object)");
  ExpectTrue("Array.isArray(result)");
  ExpectString("result[0]", "a");
  ExpectString("result[1]", "b");
  ExpectTrue("result.length === 2");
  ExpectTrue("object[Symbol.isConcatSpreadable]");

  // If access check fails, the value of @@isConcatSpreadable is ignored
  allowed_access = false;
  CompileRun("var result = [].concat(object)");
  ExpectTrue("Array.isArray(result)");
  ExpectTrue("result[0] === object");
  ExpectTrue("result.length === 1");
  ExpectTrue("object[Symbol.isConcatSpreadable] === undefined");
}


TEST(AccessCheckedToStringTag) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);
  LocalContext env;

  // Object with access check
  Local<ObjectTemplate> object_template = v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallback(AccessBlocker);
  Local<Object> object =
      object_template->NewInstance(env.local()).ToLocalChecked();

  allowed_access = true;
  env->Global()->Set(env.local(), v8_str("object"), object).FromJust();
  object->Set(env.local(), v8::Symbol::GetToStringTag(isolate), v8_str("hello"))
      .FromJust();

  // Access check is allowed, and the toStringTag is read
  CompileRun("var result = Object.prototype.toString.call(object)");
  ExpectString("result", "[object hello]");
  ExpectString("object[Symbol.toStringTag]", "hello");

  // ToString through the API should succeed too.
  String::Utf8Value result_allowed(
      isolate, object->ObjectProtoToString(env.local()).ToLocalChecked());
  CHECK_EQ(0, strcmp(*result_allowed, "[object hello]"));

  // If access check fails, the value of @@toStringTag is ignored
  allowed_access = false;
  CompileRun("var result = Object.prototype.toString.call(object)");
  ExpectString("result", "[object Object]");
  ExpectTrue("object[Symbol.toStringTag] === undefined");

  // ToString through the API should also fail.
  String::Utf8Value result_denied(
      isolate, object->ObjectProtoToString(env.local()).ToLocalChecked());
  CHECK_EQ(0, strcmp(*result_denied, "[object Object]"));
}

TEST(TemplateIteratorPrototypeIntrinsics) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  // Object templates.
  {
    Local<ObjectTemplate> object_template = v8::ObjectTemplate::New(isolate);
    object_template->SetIntrinsicDataProperty(v8_str("iter_proto"),
                                              v8::kIteratorPrototype);
    Local<Object> object =
        object_template->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), object).FromJust());
    ExpectTrue("obj.iter_proto === [][Symbol.iterator]().__proto__.__proto__");
  }
  // Setting %IteratorProto% on the function object's prototype template.
  {
    Local<FunctionTemplate> func_template = v8::FunctionTemplate::New(isolate);
    func_template->PrototypeTemplate()->SetIntrinsicDataProperty(
        v8_str("iter_proto"), v8::kIteratorPrototype);
    Local<Function> func1 =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func1"), func1).FromJust());
    Local<Function> func2 =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func2"), func2).FromJust());
    ExpectTrue(
        "func1.prototype.iter_proto === "
        "[][Symbol.iterator]().__proto__.__proto__");
    ExpectTrue(
        "func2.prototype.iter_proto === "
        "[][Symbol.iterator]().__proto__.__proto__");
    ExpectTrue("func1.prototype.iter_proto === func2.prototype.iter_proto");

    Local<Object> instance1 = func1->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("instance1"), instance1)
              .FromJust());
    ExpectFalse("instance1.hasOwnProperty('iter_proto')");
    ExpectTrue("'iter_proto' in instance1.__proto__");
    ExpectTrue(
        "instance1.iter_proto === [][Symbol.iterator]().__proto__.__proto__");
  }
  // Put %IteratorProto% in a function object's inheritance chain.
  {
    Local<FunctionTemplate> parent_template =
        v8::FunctionTemplate::New(isolate);
    parent_template->RemovePrototype();  // Remove so there is no name clash.
    parent_template->SetIntrinsicDataProperty(v8_str("prototype"),
                                              v8::kIteratorPrototype);
    Local<FunctionTemplate> func_template = v8::FunctionTemplate::New(isolate);
    func_template->Inherit(parent_template);

    Local<Function> func =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());
    ExpectTrue(
        "func.prototype.__proto__ === "
        "[][Symbol.iterator]().__proto__.__proto__");

    Local<Object> func_instance =
        func->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("instance"), func_instance)
              .FromJust());
    ExpectTrue(
        "instance.__proto__.__proto__ === "
        "[][Symbol.iterator]().__proto__.__proto__");
    ExpectTrue("instance.__proto__.__proto__.__proto__ === Object.prototype");
  }
}

TEST(TemplateErrorPrototypeIntrinsics) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  // Object templates.
  {
    Local<ObjectTemplate> object_template = v8::ObjectTemplate::New(isolate);
    object_template->SetIntrinsicDataProperty(v8_str("error_proto"),
                                              v8::kErrorPrototype);
    Local<Object> object =
        object_template->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("obj"), object).FromJust());
    ExpectTrue("obj.error_proto === Error.prototype");
    Local<Value> error = v8::Exception::Error(v8_str("error message"));
    CHECK(env->Global()->Set(env.local(), v8_str("err"), error).FromJust());
    ExpectTrue("obj.error_proto === Object.getPrototypeOf(err)");
  }
  // Setting %ErrorPrototype% on the function object's prototype template.
  {
    Local<FunctionTemplate> func_template = v8::FunctionTemplate::New(isolate);
    func_template->PrototypeTemplate()->SetIntrinsicDataProperty(
        v8_str("error_proto"), v8::kErrorPrototype);
    Local<Function> func1 =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func1"), func1).FromJust());
    Local<Function> func2 =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func2"), func2).FromJust());
    ExpectTrue("func1.prototype.error_proto === Error.prototype");
    ExpectTrue("func2.prototype.error_proto === Error.prototype");
    ExpectTrue("func1.prototype.error_proto === func2.prototype.error_proto");

    Local<Object> instance1 = func1->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("instance1"), instance1)
              .FromJust());
    ExpectFalse("instance1.hasOwnProperty('error_proto')");
    ExpectTrue("'error_proto' in instance1.__proto__");
    ExpectTrue("instance1.error_proto === Error.prototype");
  }
  // Put %ErrorPrototype% in a function object's inheritance chain.
  {
    Local<FunctionTemplate> parent_template =
        v8::FunctionTemplate::New(isolate);
    parent_template->RemovePrototype();  // Remove so there is no name clash.
    parent_template->SetIntrinsicDataProperty(v8_str("prototype"),
                                              v8::kErrorPrototype);
    Local<FunctionTemplate> func_template = v8::FunctionTemplate::New(isolate);
    func_template->Inherit(parent_template);

    Local<Function> func =
        func_template->GetFunction(env.local()).ToLocalChecked();
    CHECK(env->Global()->Set(env.local(), v8_str("func"), func).FromJust());
    ExpectTrue("func.prototype.__proto__ === Error.prototype");

    Local<Object> func_instance =
        func->NewInstance(env.local()).ToLocalChecked();
    CHECK(env->Global()
              ->Set(env.local(), v8_str("instance"), func_instance)
              .FromJust());
    ExpectTrue("instance.__proto__.__proto__.__proto__ === Object.prototype");
    // Now let's check if %ErrorPrototype% properties are in the instance.
    ExpectTrue("'constructor' in instance");
    ExpectTrue("'message' in instance");
    ExpectTrue("'name' in instance");
    ExpectTrue("'toString' in instance");
  }
}

TEST(ObjectTemplateArrayProtoIntrinsics) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  Local<ObjectTemplate> object_template = v8::ObjectTemplate::New(isolate);
  object_template->SetIntrinsicDataProperty(v8_str("prop_entries"),
                                            v8::kArrayProto_entries);
  object_template->SetIntrinsicDataProperty(v8_str("prop_forEach"),
                                            v8::kArrayProto_forEach);
  object_template->SetIntrinsicDataProperty(v8_str("prop_keys"),
                                            v8::kArrayProto_keys);
  object_template->SetIntrinsicDataProperty(v8_str("prop_values"),
                                            v8::kArrayProto_values);
  Local<Object> object =
      object_template->NewInstance(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("obj1"), object).FromJust());

  const struct {
    const char* const object_property_name;
    const char* const array_property_name;
  } intrinsics_comparisons[] = {
      {"prop_entries", "Array.prototype.entries"},
      {"prop_forEach", "Array.prototype.forEach"},
      {"prop_keys", "Array.prototype.keys"},
      {"prop_values", "Array.prototype[Symbol.iterator]"},
  };

  for (unsigned i = 0; i < arraysize(intrinsics_comparisons); i++) {
    i::ScopedVector<char> test_string(64);

    i::SNPrintF(test_string, "typeof obj1.%s",
                intrinsics_comparisons[i].object_property_name);
    ExpectString(test_string.begin(), "function");

    i::SNPrintF(test_string, "obj1.%s === %s",
                intrinsics_comparisons[i].object_property_name,
                intrinsics_comparisons[i].array_property_name);
    ExpectTrue(test_string.begin());

    i::SNPrintF(test_string, "obj1.%s = 42",
                intrinsics_comparisons[i].object_property_name);
    CompileRun(test_string.begin());

    i::SNPrintF(test_string, "obj1.%s === %s",
                intrinsics_comparisons[i].object_property_name,
                intrinsics_comparisons[i].array_property_name);
    ExpectFalse(test_string.begin());

    i::SNPrintF(test_string, "typeof obj1.%s",
                intrinsics_comparisons[i].object_property_name);
    ExpectString(test_string.begin(), "number");
  }
}

TEST(ObjectTemplatePerContextIntrinsics) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  Local<ObjectTemplate> object_template = v8::ObjectTemplate::New(isolate);
  object_template->SetIntrinsicDataProperty(v8_str("values"),
                                            v8::kArrayProto_values);
  Local<Object> object =
      object_template->NewInstance(env.local()).ToLocalChecked();

  CHECK(env->Global()->Set(env.local(), v8_str("obj1"), object).FromJust());
  ExpectString("typeof obj1.values", "function");

  auto values = Local<Function>::Cast(
      object->Get(env.local(), v8_str("values")).ToLocalChecked());
  auto fn = i::Handle<i::JSFunction>::cast(v8::Utils::OpenHandle(*values));
  auto ctx = v8::Utils::OpenHandle(*env.local());
  CHECK_EQ(*fn->GetCreationContext(), *ctx);

  {
    LocalContext env2;
    Local<Object> object2 =
        object_template->NewInstance(env2.local()).ToLocalChecked();
    CHECK(
        env2->Global()->Set(env2.local(), v8_str("obj2"), object2).FromJust());
    ExpectString("typeof obj2.values", "function");
    CHECK_NE(*object->Get(env2.local(), v8_str("values")).ToLocalChecked(),
             *object2->Get(env2.local(), v8_str("values")).ToLocalChecked());

    auto values2 = Local<Function>::Cast(
        object2->Get(env2.local(), v8_str("values")).ToLocalChecked());
    auto fn2 = i::Handle<i::JSFunction>::cast(v8::Utils::OpenHandle(*values2));
    auto ctx2 = v8::Utils::OpenHandle(*env2.local());
    CHECK_EQ(*fn2->GetCreationContext(), *ctx2);
  }
}


TEST(Proxy) {
  LocalContext context;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Object> target = CompileRun("({})").As<v8::Object>();
  v8::Local<v8::Object> handler = CompileRun("({})").As<v8::Object>();

  v8::Local<v8::Proxy> proxy =
      v8::Proxy::New(context.local(), target, handler).ToLocalChecked();
  CHECK(proxy->IsProxy());
  CHECK(!target->IsProxy());
  CHECK(!proxy->IsRevoked());
  CHECK(proxy->GetTarget()->SameValue(target));
  CHECK(proxy->GetHandler()->SameValue(handler));

  proxy->Revoke();
  CHECK(proxy->IsProxy());
  CHECK(!target->IsProxy());
  CHECK(proxy->IsRevoked());
  CHECK(proxy->GetTarget()->IsNull());
  CHECK(proxy->GetHandler()->IsNull());
}

WeakCallCounterAndPersistent<Value>* CreateGarbageWithWeakCallCounter(
    v8::Isolate* isolate, WeakCallCounter* counter) {
  v8::Locker locker(isolate);
  LocalContext env;
  HandleScope scope(isolate);
  WeakCallCounterAndPersistent<Value>* val =
      new WeakCallCounterAndPersistent<Value>(counter);
  val->handle.Reset(isolate, Object::New(isolate));
  val->handle.SetWeak(val, &WeakPointerCallback,
                      v8::WeakCallbackType::kParameter);
  return val;
}

class MemoryPressureThread : public v8::base::Thread {
 public:
  explicit MemoryPressureThread(v8::Isolate* isolate,
                                v8::MemoryPressureLevel level)
      : Thread(Options("MemoryPressureThread")),
        isolate_(isolate),
        level_(level) {}

  void Run() override { isolate_->MemoryPressureNotification(level_); }

 private:
  v8::Isolate* isolate_;
  v8::MemoryPressureLevel level_;
};

TEST(MemoryPressure) {
  if (v8::internal::FLAG_optimize_for_size) return;
  v8::Isolate* isolate = CcTest::isolate();
  WeakCallCounter counter(1234);

  // Check that critical memory pressure notification sets GC interrupt.
  auto garbage = CreateGarbageWithWeakCallCounter(isolate, &counter);
  CHECK(!v8::Locker::IsLocked(isolate));
  {
    v8::Locker locker(isolate);
    v8::HandleScope scope(isolate);
    LocalContext env;
    MemoryPressureThread memory_pressure_thread(
        isolate, v8::MemoryPressureLevel::kCritical);
    CHECK(memory_pressure_thread.Start());
    memory_pressure_thread.Join();
    // This should trigger GC.
    CHECK_EQ(0, counter.NumberOfWeakCalls());
    CompileRun("(function noop() { return 0; })()");
    CHECK_EQ(1, counter.NumberOfWeakCalls());
  }
  delete garbage;
  // Check that critical memory pressure notification triggers GC.
  garbage = CreateGarbageWithWeakCallCounter(isolate, &counter);
  {
    v8::Locker locker(isolate);
    // If isolate is locked, memory pressure notification should trigger GC.
    CHECK_EQ(1, counter.NumberOfWeakCalls());
    isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
    CHECK_EQ(2, counter.NumberOfWeakCalls());
  }
  delete garbage;
  // Check that moderate memory pressure notification sets GC into memory
  // optimizing mode.
  isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kModerate);
  CHECK(CcTest::i_isolate()->heap()->ShouldOptimizeForMemoryUsage());
  // Check that disabling memory pressure returns GC into normal mode.
  isolate->MemoryPressureNotification(v8::MemoryPressureLevel::kNone);
  CHECK(!CcTest::i_isolate()->heap()->ShouldOptimizeForMemoryUsage());
}

TEST(SetIntegrityLevel) {
  LocalContext context;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  CHECK(context->Global()->Set(context.local(), v8_str("o"), obj).FromJust());

  v8::Local<v8::Value> is_frozen = CompileRun("Object.isFrozen(o)");
  CHECK(!is_frozen->BooleanValue(isolate));

  CHECK(obj->SetIntegrityLevel(context.local(), v8::IntegrityLevel::kFrozen)
            .FromJust());

  is_frozen = CompileRun("Object.isFrozen(o)");
  CHECK(is_frozen->BooleanValue(isolate));
}

TEST(PrivateForApiIsNumber) {
  LocalContext context;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Shouldn't crash.
  v8::Private::ForApi(isolate, v8_str("42"));
}

THREADED_TEST(ImmutableProto) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->InstanceTemplate()->SetImmutableProto();

  Local<v8::Object> object = templ->GetFunction(context.local())
                                 .ToLocalChecked()
                                 ->NewInstance(context.local())
                                 .ToLocalChecked();

  // Look up the prototype
  Local<v8::Value> original_proto =
      object->Get(context.local(), v8_str("__proto__")).ToLocalChecked();

  // Setting the prototype (e.g., to null) throws
  CHECK(object->SetPrototype(context.local(), v8::Null(isolate)).IsNothing());

  // The original prototype is still there
  Local<Value> new_proto =
      object->Get(context.local(), v8_str("__proto__")).ToLocalChecked();
  CHECK(new_proto->IsObject());
  CHECK(new_proto.As<v8::Object>()
            ->Equals(context.local(), original_proto)
            .FromJust());
}

Local<v8::Context> call_eval_context;
Local<v8::Function> call_eval_bound_function;

static void CallEval(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Context::Scope scope(call_eval_context);
  args.GetReturnValue().Set(
      call_eval_bound_function
          ->Call(call_eval_context, call_eval_context->Global(), 0, nullptr)
          .ToLocalChecked());
}

TEST(CrossActivationEval) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  {
    call_eval_context = v8::Context::New(isolate);
    v8::Context::Scope scope(call_eval_context);
    call_eval_bound_function =
        Local<Function>::Cast(CompileRun("eval.bind(this, '1')"));
  }
  env->Global()
      ->Set(env.local(), v8_str("CallEval"),
            v8::FunctionTemplate::New(isolate, CallEval)
                ->GetFunction(env.local())
                .ToLocalChecked())
      .FromJust();
  Local<Value> result = CompileRun("CallEval();");
  CHECK(result->IsInt32());
  CHECK_EQ(1, result->Int32Value(env.local()).FromJust());
}

TEST(EvalInAccessCheckedContext) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> obj_template = v8::ObjectTemplate::New(isolate);

  obj_template->SetAccessCheckCallback(AccessAlwaysAllowed);

  v8::Local<Context> context0 = Context::New(isolate, nullptr, obj_template);
  v8::Local<Context> context1 = Context::New(isolate, nullptr, obj_template);

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to different domains.
  context0->SetSecurityToken(foo);
  context1->SetSecurityToken(bar);

  // Set up function in context0 that uses eval from context0.
  context0->Enter();
  v8::Local<v8::Value> fun = CompileRun(
      "var x = 42;"
      "(function() {"
      "  var e = eval;"
      "  return function(s) { return e(s); }"
      "})()");
  context0->Exit();

  // Put the function into context1 and call it. Since the access check
  // callback always returns true, the call succeeds even though the tokens
  // are different.
  context1->Enter();
  context1->Global()->Set(context1, v8_str("fun"), fun).FromJust();
  v8::Local<v8::Value> x_value = CompileRun("fun('x')");
  CHECK_EQ(42, x_value->Int32Value(context1).FromJust());
  context1->Exit();
}

THREADED_TEST(ImmutableProtoWithParent) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> parent = v8::FunctionTemplate::New(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->Inherit(parent);
  templ->PrototypeTemplate()->SetImmutableProto();

  Local<v8::Function> function =
      templ->GetFunction(context.local()).ToLocalChecked();
  Local<v8::Object> instance =
      function->NewInstance(context.local()).ToLocalChecked();
  Local<v8::Object> prototype =
      instance->Get(context.local(), v8_str("__proto__"))
          .ToLocalChecked()
          ->ToObject(context.local())
          .ToLocalChecked();

  // Look up the prototype
  Local<v8::Value> original_proto =
      prototype->Get(context.local(), v8_str("__proto__")).ToLocalChecked();

  // Setting the prototype (e.g., to null) throws
  CHECK(
      prototype->SetPrototype(context.local(), v8::Null(isolate)).IsNothing());

  // The original prototype is still there
  Local<Value> new_proto =
      prototype->Get(context.local(), v8_str("__proto__")).ToLocalChecked();
  CHECK(new_proto->IsObject());
  CHECK(new_proto.As<v8::Object>()
            ->Equals(context.local(), original_proto)
            .FromJust());
}

TEST(InternalFieldsOnGlobalProxy) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> obj_template = v8::ObjectTemplate::New(isolate);
  obj_template->SetInternalFieldCount(1);

  v8::Local<v8::Context> context = Context::New(isolate, nullptr, obj_template);
  v8::Local<v8::Object> global = context->Global();
  CHECK_EQ(1, global->InternalFieldCount());
}

THREADED_TEST(ImmutableProtoGlobal) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  Local<ObjectTemplate> global_template = ObjectTemplate::New(isolate);
  global_template->SetImmutableProto();
  v8::Local<Context> context = Context::New(isolate, nullptr, global_template);
  Context::Scope context_scope(context);
  v8::Local<Value> result = CompileRun(
      "global = this;"
      "(function() {"
      "  try {"
      "    global.__proto__ = {};"
      "    return 0;"
      "  } catch (e) {"
      "    return 1;"
      "  }"
      "})()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 1))
            .FromJust());
}

THREADED_TEST(MutableProtoGlobal) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  Local<ObjectTemplate> global_template = ObjectTemplate::New(isolate);
  v8::Local<Context> context = Context::New(isolate, nullptr, global_template);
  Context::Scope context_scope(context);
  v8::Local<Value> result = CompileRun(
      "global = this;"
      "(function() {"
      "  try {"
      "    global.__proto__ = {};"
      "    return 0;"
      "  } catch (e) {"
      "    return 1;"
      "  }"
      "})()");
  CHECK(result->Equals(context, v8::Integer::New(CcTest::isolate(), 0))
            .FromJust());
}

TEST(SetPrototypeTemplate) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<FunctionTemplate> HTMLElementTemplate = FunctionTemplate::New(isolate);
  Local<FunctionTemplate> HTMLImageElementTemplate =
      FunctionTemplate::New(isolate);
  HTMLImageElementTemplate->Inherit(HTMLElementTemplate);

  Local<FunctionTemplate> ImageTemplate = FunctionTemplate::New(isolate);
  ImageTemplate->SetPrototypeProviderTemplate(HTMLImageElementTemplate);

  Local<Function> HTMLImageElement =
      HTMLImageElementTemplate->GetFunction(env.local()).ToLocalChecked();
  Local<Function> Image =
      ImageTemplate->GetFunction(env.local()).ToLocalChecked();

  CHECK(env->Global()
            ->Set(env.local(), v8_str("HTMLImageElement"), HTMLImageElement)
            .FromJust());
  CHECK(env->Global()->Set(env.local(), v8_str("Image"), Image).FromJust());

  ExpectTrue("Image.prototype === HTMLImageElement.prototype");
}

void ensure_receiver_is_global_proxy(
    v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSGlobalProxy());
}

THREADED_TEST(GlobalAccessorInfo) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  global_template->SetAccessor(
      v8::String::NewFromUtf8(isolate, "prop", v8::NewStringType::kInternalized)
          .ToLocalChecked(),
      &ensure_receiver_is_global_proxy);
  LocalContext env(nullptr, global_template);
  CompileRun("for (var i = 0; i < 10; i++) this.prop");
  CompileRun("for (var i = 0; i < 10; i++) prop");
}

TEST(DeterministicRandomNumberGeneration) {
  v8::HandleScope scope(CcTest::isolate());

  int previous_seed = v8::internal::FLAG_random_seed;
  v8::internal::FLAG_random_seed = 1234;

  double first_value;
  double second_value;
  {
    v8::Local<Context> context = Context::New(CcTest::isolate());
    Context::Scope context_scope(context);
    v8::Local<Value> result = CompileRun("Math.random();");
    first_value = result->ToNumber(context).ToLocalChecked()->Value();
  }
  {
    v8::Local<Context> context = Context::New(CcTest::isolate());
    Context::Scope context_scope(context);
    v8::Local<Value> result = CompileRun("Math.random();");
    second_value = result->ToNumber(context).ToLocalChecked()->Value();
  }
  CHECK_EQ(first_value, second_value);

  v8::internal::FLAG_random_seed = previous_seed;
}

UNINITIALIZED_TEST(AllowAtomicsWait) {
  v8::Isolate::CreateParams create_params;
  create_params.allow_atomics_wait = false;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  {
    CHECK_EQ(false, i_isolate->allow_atomics_wait());
    isolate->SetAllowAtomicsWait(true);
    CHECK_EQ(true, i_isolate->allow_atomics_wait());
  }
  isolate->Dispose();
}

enum ContextId { EnteredContext, CurrentContext };

void CheckContexts(v8::Isolate* isolate) {
  CHECK_EQ(CurrentContext, isolate->GetCurrentContext()
                               ->GetEmbedderData(1)
                               .As<v8::Integer>()
                               ->Value());
  CHECK_EQ(EnteredContext, isolate->GetEnteredOrMicrotaskContext()
                               ->GetEmbedderData(1)
                               .As<v8::Integer>()
                               ->Value());
}

void ContextCheckGetter(Local<String> name,
                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  CheckContexts(info.GetIsolate());
  info.GetReturnValue().Set(true);
}

void ContextCheckSetter(Local<String> name, Local<Value>,
                        const v8::PropertyCallbackInfo<void>& info) {
  CheckContexts(info.GetIsolate());
}

void ContextCheckToString(const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckContexts(info.GetIsolate());
  info.GetReturnValue().Set(v8_str("foo"));
}

TEST(CorrectEnteredContext) {
  v8::HandleScope scope(CcTest::isolate());

  LocalContext currentContext;
  currentContext->SetEmbedderData(
      1, v8::Integer::New(currentContext->GetIsolate(), CurrentContext));
  LocalContext enteredContext;
  enteredContext->SetEmbedderData(
      1, v8::Integer::New(enteredContext->GetIsolate(), EnteredContext));

  v8::Context::Scope contextScope(enteredContext.local());

  v8::Local<v8::ObjectTemplate> object_template =
      ObjectTemplate::New(currentContext->GetIsolate());
  object_template->SetAccessor(v8_str("p"), &ContextCheckGetter,
                               &ContextCheckSetter);

  v8::Local<v8::Object> object =
      object_template->NewInstance(currentContext.local()).ToLocalChecked();

  object->Get(currentContext.local(), v8_str("p")).ToLocalChecked();
  object->Set(currentContext.local(), v8_str("p"), v8_int(0)).FromJust();

  v8::Local<v8::Function> to_string =
      v8::Function::New(currentContext.local(), ContextCheckToString)
          .ToLocalChecked();

  to_string->Call(currentContext.local(), object, 0, nullptr).ToLocalChecked();

  object
      ->CreateDataProperty(currentContext.local(), v8_str("toString"),
                           to_string)
      .FromJust();

  object->ToString(currentContext.local()).ToLocalChecked();
}

v8::MaybeLocal<v8::Promise> HostImportModuleDynamicallyCallbackResolve(
    Local<Context> context, Local<v8::ScriptOrModule> referrer,
    Local<String> specifier) {
  CHECK(!referrer.IsEmpty());
  String::Utf8Value referrer_utf8(
      context->GetIsolate(), Local<String>::Cast(referrer->GetResourceName()));
  CHECK_EQ(0, strcmp("www.google.com", *referrer_utf8));
  CHECK(referrer->GetHostDefinedOptions()
            ->Get(context->GetIsolate(), 0)
            ->IsSymbol());

  CHECK(!specifier.IsEmpty());
  String::Utf8Value specifier_utf8(context->GetIsolate(), specifier);
  CHECK_EQ(0, strcmp("index.js", *specifier_utf8));

  Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  auto result = v8_str("hello world");
  resolver->Resolve(context, result).ToChecked();
  return resolver->GetPromise();
}

TEST(DynamicImport) {
  i::FLAG_harmony_dynamic_import = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetHostImportModuleDynamicallyCallback(
      HostImportModuleDynamicallyCallbackResolve);

  i::Handle<i::String> url(v8::Utils::OpenHandle(*v8_str("www.google.com")));
  i::Handle<i::Object> specifier(v8::Utils::OpenHandle(*v8_str("index.js")));
  i::Handle<i::String> result(v8::Utils::OpenHandle(*v8_str("hello world")));
  i::Handle<i::String> source(v8::Utils::OpenHandle(*v8_str("foo")));
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Handle<i::FixedArray> options = i_isolate->factory()->NewFixedArray(1);
  i::Handle<i::Symbol> symbol = i_isolate->factory()->NewSymbol();
  options->set(0, *symbol);
  i::Handle<i::Script> referrer = i_isolate->factory()->NewScript(source);
  referrer->set_name(*url);
  referrer->set_host_defined_options(*options);
  i::MaybeHandle<i::JSPromise> maybe_promise =
      i_isolate->RunHostImportModuleDynamicallyCallback(referrer, specifier);
  i::Handle<i::JSPromise> promise = maybe_promise.ToHandleChecked();
  isolate->RunMicrotasks();
  CHECK(result->Equals(i::String::cast(promise->result())));
}

void HostInitializeImportMetaObjectCallbackStatic(Local<Context> context,
                                                  Local<Module> module,
                                                  Local<Object> meta) {
  CHECK(!module.IsEmpty());

  meta->CreateDataProperty(context, v8_str("foo"), v8_str("bar")).ToChecked();
}

TEST(ImportMeta) {
  i::FLAG_harmony_dynamic_import = true;
  i::FLAG_harmony_import_meta = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetHostInitializeImportMetaObjectCallback(
      HostInitializeImportMetaObjectCallbackStatic);

  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  Local<String> url = v8_str("www.google.com");
  Local<String> source_text = v8_str("import.meta;");
  v8::ScriptOrigin origin(url, Local<v8::Integer>(), Local<v8::Integer>(),
                          Local<v8::Boolean>(), Local<v8::Integer>(),
                          Local<v8::Value>(), Local<v8::Boolean>(),
                          Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(source_text, origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
  i::Handle<i::Object> meta =
      i_isolate->RunHostInitializeImportMetaObjectCallback(
          i::Handle<i::SourceTextModule>::cast(v8::Utils::OpenHandle(*module)));
  CHECK(meta->IsJSObject());
  Local<Object> meta_obj = Local<Object>::Cast(v8::Utils::ToLocal(meta));
  CHECK(meta_obj->Get(context.local(), v8_str("foo"))
            .ToLocalChecked()
            ->IsString());
  CHECK(meta_obj->Get(context.local(), v8_str("zapp"))
            .ToLocalChecked()
            ->IsUndefined());

  module->InstantiateModule(context.local(), UnexpectedModuleResolveCallback)
      .ToChecked();
  Local<Value> result = module->Evaluate(context.local()).ToLocalChecked();
  CHECK(result->StrictEquals(Local<v8::Value>::Cast(v8::Utils::ToLocal(meta))));
}

TEST(GetModuleNamespace) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<String> url = v8_str("www.google.com");
  Local<String> source_text = v8_str("export default 5; export const a = 10;");
  v8::ScriptOrigin origin(url, Local<v8::Integer>(), Local<v8::Integer>(),
                          Local<v8::Boolean>(), Local<v8::Integer>(),
                          Local<v8::Value>(), Local<v8::Boolean>(),
                          Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(source_text, origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
  module->InstantiateModule(context.local(), UnexpectedModuleResolveCallback)
      .ToChecked();
  module->Evaluate(context.local()).ToLocalChecked();

  Local<Value> ns_val = module->GetModuleNamespace();
  CHECK(ns_val->IsModuleNamespaceObject());
  Local<Object> ns = ns_val.As<Object>();
  CHECK(ns->Get(context.local(), v8_str("default"))
            .ToLocalChecked()
            ->StrictEquals(v8::Number::New(isolate, 5)));
  CHECK(ns->Get(context.local(), v8_str("a"))
            .ToLocalChecked()
            ->StrictEquals(v8::Number::New(isolate, 10)));
}

TEST(ModuleGetUnboundModuleScript) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<String> url = v8_str("www.google.com");
  Local<String> source_text = v8_str("export default 5; export const a = 10;");
  v8::ScriptOrigin origin(url, Local<v8::Integer>(), Local<v8::Integer>(),
                          Local<v8::Boolean>(), Local<v8::Integer>(),
                          Local<v8::Value>(), Local<v8::Boolean>(),
                          Local<v8::Boolean>(), True(isolate));
  v8::ScriptCompiler::Source source(source_text, origin);
  Local<Module> module =
      v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
  Local<v8::UnboundModuleScript> sfi_before_instantiation =
      module->GetUnboundModuleScript();
  module->InstantiateModule(context.local(), UnexpectedModuleResolveCallback)
      .ToChecked();
  Local<v8::UnboundModuleScript> sfi_after_instantiation =
      module->GetUnboundModuleScript();

  // Check object identity.
  {
    i::Handle<i::Object> s1 = v8::Utils::OpenHandle(*sfi_before_instantiation);
    i::Handle<i::Object> s2 = v8::Utils::OpenHandle(*sfi_after_instantiation);
    CHECK_EQ(*s1, *s2);
  }
}

TEST(GlobalTemplateWithDoubleProperty) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
  global->Set(v8_str("double"), v8_num(3.14));

  v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global);

  v8::Context::Scope context_scope(context);

  Local<Value> result = CompileRun("double");
  CHECK(result->IsNumber());
  CheckDoubleEquals(3.14, result->NumberValue(context).ToChecked());
}

TEST(PrimitiveArray) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  int length = 5;
  Local<v8::PrimitiveArray> array(v8::PrimitiveArray::New(isolate, 5));
  CHECK_EQ(length, array->Length());

  for (int i = 0; i < length; i++) {
    Local<v8::Primitive> item = array->Get(isolate, i);
    CHECK(item->IsUndefined());
  }

  Local<v8::Symbol> symbol(v8::Symbol::New(isolate));
  array->Set(isolate, 0, symbol);
  CHECK(array->Get(isolate, 0)->IsSymbol());

  Local<v8::String> string =
      v8::String::NewFromUtf8(isolate, "test", v8::NewStringType::kInternalized)
          .ToLocalChecked();
  array->Set(isolate, 1, string);
  CHECK(array->Get(isolate, 0)->IsSymbol());
  CHECK(array->Get(isolate, 1)->IsString());

  Local<v8::Number> num = v8::Number::New(env->GetIsolate(), 3.1415926);
  array->Set(isolate, 2, num);
  CHECK(array->Get(isolate, 0)->IsSymbol());
  CHECK(array->Get(isolate, 1)->IsString());
  CHECK(array->Get(isolate, 2)->IsNumber());

  v8::Local<v8::Boolean> f = v8::False(isolate);
  array->Set(isolate, 3, f);
  CHECK(array->Get(isolate, 0)->IsSymbol());
  CHECK(array->Get(isolate, 1)->IsString());
  CHECK(array->Get(isolate, 2)->IsNumber());
  CHECK(array->Get(isolate, 3)->IsBoolean());

  v8::Local<v8::Primitive> n = v8::Null(isolate);
  array->Set(isolate, 4, n);
  CHECK(array->Get(isolate, 0)->IsSymbol());
  CHECK(array->Get(isolate, 1)->IsString());
  CHECK(array->Get(isolate, 2)->IsNumber());
  CHECK(array->Get(isolate, 3)->IsBoolean());
  CHECK(array->Get(isolate, 4)->IsNull());
}

TEST(PersistentValueMap) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;

  v8::PersistentValueMap<
      std::string, v8::Value,
      v8::DefaultPersistentValueMapTraits<std::string, v8::Value>>
      map(isolate);
  v8::Local<v8::Value> value =
      v8::String::NewFromUtf8(isolate, "value",
                              v8::NewStringType::kInternalized)
          .ToLocalChecked();
  map.Set("key", value);
}

namespace {

bool wasm_streaming_callback_got_called = false;
bool wasm_streaming_data_got_collected = false;

void WasmStreamingTestFinalizer(const v8::WeakCallbackInfo<void>& data) {
  CHECK(!wasm_streaming_data_got_collected);
  wasm_streaming_data_got_collected = true;
  i::GlobalHandles::Destroy(reinterpret_cast<i::Address*>(data.GetParameter()));
}

void WasmStreamingCallbackTestCallbackIsCalled(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK(!wasm_streaming_callback_got_called);
  wasm_streaming_callback_got_called = true;

  i::Handle<i::Object> global_handle =
      reinterpret_cast<i::Isolate*>(args.GetIsolate())
          ->global_handles()
          ->Create(*v8::Utils::OpenHandle(*args.Data()));
  i::GlobalHandles::MakeWeak(global_handle.location(), global_handle.location(),
                             WasmStreamingTestFinalizer,
                             v8::WeakCallbackType::kParameter);
}

void WasmStreamingCallbackTestOnBytesReceived(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());

  // The first bytes of the WebAssembly magic word.
  const uint8_t bytes[]{0x00, 0x61, 0x73};
  streaming->OnBytesReceived(bytes, arraysize(bytes));
}

void WasmStreamingCallbackTestFinishWithSuccess(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  // The bytes of a minimal WebAssembly module.
  const uint8_t bytes[]{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  streaming->OnBytesReceived(bytes, arraysize(bytes));
  streaming->Finish();
}

void WasmStreamingCallbackTestFinishWithFailure(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->Finish();
}

void WasmStreamingCallbackTestAbortWithReject(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->Abort(v8::Object::New(args.GetIsolate()));
}

void WasmStreamingCallbackTestAbortNoReject(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->Abort({});
}

void TestWasmStreaming(v8::WasmStreamingCallback callback,
                       v8::Promise::PromiseState expected_state) {
  CcTest::isolate()->SetWasmStreamingCallback(callback);
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // Call {WebAssembly.compileStreaming} with {null} as parameter. The parameter
  // is only really processed by the embedder, so for this test the value is
  // irrelevant.
  v8::Local<v8::Promise> promise = v8::Local<v8::Promise>::Cast(
      CompileRun("WebAssembly.compileStreaming(null)"));

  EmptyMessageQueues(isolate);
  CHECK_EQ(expected_state, promise->State());
}

}  // namespace

TEST(WasmStreamingCallback) {
  TestWasmStreaming(WasmStreamingCallbackTestCallbackIsCalled,
                    v8::Promise::kPending);
  CHECK(wasm_streaming_callback_got_called);
  CcTest::CollectAllAvailableGarbage();
  CHECK(wasm_streaming_data_got_collected);
}

TEST(WasmStreamingOnBytesReceived) {
  TestWasmStreaming(WasmStreamingCallbackTestOnBytesReceived,
                    v8::Promise::kPending);
}

TEST(WasmStreamingFinishWithSuccess) {
  TestWasmStreaming(WasmStreamingCallbackTestFinishWithSuccess,
                    v8::Promise::kFulfilled);
}

TEST(WasmStreamingFinishWithFailure) {
  TestWasmStreaming(WasmStreamingCallbackTestFinishWithFailure,
                    v8::Promise::kRejected);
}

TEST(WasmStreamingAbortWithReject) {
  TestWasmStreaming(WasmStreamingCallbackTestAbortWithReject,
                    v8::Promise::kRejected);
}

TEST(WasmStreamingAbortWithoutReject) {
  TestWasmStreaming(WasmStreamingCallbackTestAbortNoReject,
                    v8::Promise::kPending);
}

enum class AtomicsWaitCallbackAction {
  Interrupt,
  StopAndThrowInFirstCall,
  StopAndThrowInSecondCall,
  StopFromThreadAndThrow,
  KeepWaiting
};

class StopAtomicsWaitThread;

struct AtomicsWaitCallbackInfo {
  v8::Isolate* isolate;
  v8::Isolate::AtomicsWaitWakeHandle* wake_handle;
  std::unique_ptr<StopAtomicsWaitThread> stop_thread;
  AtomicsWaitCallbackAction action;

  Local<v8::SharedArrayBuffer> expected_sab;
  v8::Isolate::AtomicsWaitEvent expected_event;
  double expected_timeout;
  int64_t expected_value;
  size_t expected_offset;

  size_t ncalls = 0;
};

class StopAtomicsWaitThread : public v8::base::Thread {
 public:
  explicit StopAtomicsWaitThread(AtomicsWaitCallbackInfo* info)
      : Thread(Options("StopAtomicsWaitThread")), info_(info) {}

  void Run() override {
    CHECK_NOT_NULL(info_->wake_handle);
    info_->wake_handle->Wake();
  }

 private:
  AtomicsWaitCallbackInfo* info_;
};

void AtomicsWaitCallbackForTesting(
    v8::Isolate::AtomicsWaitEvent event, Local<v8::SharedArrayBuffer> sab,
    size_t offset_in_bytes, int64_t value, double timeout_in_ms,
    v8::Isolate::AtomicsWaitWakeHandle* wake_handle, void* data) {
  AtomicsWaitCallbackInfo* info = static_cast<AtomicsWaitCallbackInfo*>(data);
  info->ncalls++;
  info->wake_handle = wake_handle;
  CHECK(sab->StrictEquals(info->expected_sab));
  CHECK_EQ(timeout_in_ms, info->expected_timeout);
  CHECK_EQ(value, info->expected_value);
  CHECK_EQ(offset_in_bytes, info->expected_offset);

  auto ThrowSomething = [&]() {
    info->isolate->ThrowException(v8::Integer::New(info->isolate, 42));
  };

  if (event == v8::Isolate::AtomicsWaitEvent::kStartWait) {
    CHECK_NOT_NULL(wake_handle);
    switch (info->action) {
      case AtomicsWaitCallbackAction::Interrupt:
        info->isolate->TerminateExecution();
        break;
      case AtomicsWaitCallbackAction::StopAndThrowInFirstCall:
        ThrowSomething();
        V8_FALLTHROUGH;
      case AtomicsWaitCallbackAction::StopAndThrowInSecondCall:
        wake_handle->Wake();
        break;
      case AtomicsWaitCallbackAction::StopFromThreadAndThrow:
        info->stop_thread = v8::base::make_unique<StopAtomicsWaitThread>(info);
        CHECK(info->stop_thread->Start());
        break;
      case AtomicsWaitCallbackAction::KeepWaiting:
        break;
    }
  } else {
    CHECK_EQ(event, info->expected_event);
    CHECK_NULL(wake_handle);

    if (info->stop_thread) {
      info->stop_thread->Join();
      info->stop_thread.reset();
    }

    if (info->action == AtomicsWaitCallbackAction::StopAndThrowInSecondCall ||
        info->action == AtomicsWaitCallbackAction::StopFromThreadAndThrow) {
      ThrowSomething();
    }
  }
}

// Must be called from within HandleScope
void AtomicsWaitCallbackCommon(v8::Isolate* isolate, Local<Value> sab,
                               size_t initial_offset,
                               size_t offset_multiplier) {
  CHECK(sab->IsSharedArrayBuffer());

  AtomicsWaitCallbackInfo info;
  info.isolate = isolate;
  info.expected_sab = sab.As<v8::SharedArrayBuffer>();
  isolate->SetAtomicsWaitCallback(AtomicsWaitCallbackForTesting, &info);

  {
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 0;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kTerminatedExecution;
    info.action = AtomicsWaitCallbackAction::Interrupt;
    info.ncalls = 0;
    CompileRun("wait(0, 0);");
    CHECK_EQ(info.ncalls, 2);
    CHECK(try_catch.HasTerminated());
  }

  {
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset + offset_multiplier;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 1;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kNotEqual;
    info.action = AtomicsWaitCallbackAction::KeepWaiting;
    info.ncalls = 0;
    CompileRun("wait(1, 1);");  // real value is 0 != 1
    CHECK_EQ(info.ncalls, 2);
    CHECK(!try_catch.HasCaught());
  }

  {
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset + offset_multiplier;
    info.expected_timeout = 0.125;
    info.expected_value = 0;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kTimedOut;
    info.action = AtomicsWaitCallbackAction::KeepWaiting;
    info.ncalls = 0;
    CompileRun("wait(1, 0, 0.125);");  // timeout
    CHECK_EQ(info.ncalls, 2);
    CHECK(!try_catch.HasCaught());
  }

  {
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset + offset_multiplier;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 0;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kAPIStopped;
    info.action = AtomicsWaitCallbackAction::StopAndThrowInFirstCall;
    info.ncalls = 0;
    CompileRun("wait(1, 0);");
    CHECK_EQ(info.ncalls, 1);  // Only one extra call
    CHECK(try_catch.HasCaught());
    CHECK(try_catch.Exception()->IsInt32());
    CHECK_EQ(try_catch.Exception().As<v8::Int32>()->Value(), 42);
  }

  {
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset + offset_multiplier;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 0;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kAPIStopped;
    info.action = AtomicsWaitCallbackAction::StopAndThrowInSecondCall;
    info.ncalls = 0;
    CompileRun("wait(1, 0);");
    CHECK_EQ(info.ncalls, 2);
    CHECK(try_catch.HasCaught());
    CHECK(try_catch.Exception()->IsInt32());
    CHECK_EQ(try_catch.Exception().As<v8::Int32>()->Value(), 42);
  }

  {
    // Same test as before, but with a different `expected_value`.
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset + offset_multiplier;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 200;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kAPIStopped;
    info.action = AtomicsWaitCallbackAction::StopAndThrowInSecondCall;
    info.ncalls = 0;
    CompileRun(
        "setArrayElemAs(1, 200);"
        "wait(1, 200);");
    CHECK_EQ(info.ncalls, 2);
    CHECK(try_catch.HasCaught());
    CHECK(try_catch.Exception()->IsInt32());
    CHECK_EQ(try_catch.Exception().As<v8::Int32>()->Value(), 42);
  }

  {
    // Wake the `Atomics.wait()` call from a thread.
    v8::TryCatch try_catch(isolate);
    info.expected_offset = initial_offset;
    info.expected_timeout = std::numeric_limits<double>::infinity();
    info.expected_value = 0;
    info.expected_event = v8::Isolate::AtomicsWaitEvent::kAPIStopped;
    info.action = AtomicsWaitCallbackAction::StopFromThreadAndThrow;
    info.ncalls = 0;
    CompileRun(
        "setArrayElemAs(1, 0);"
        "wait(0, 0);");
    CHECK_EQ(info.ncalls, 2);
    CHECK(try_catch.HasCaught());
    CHECK(try_catch.Exception()->IsInt32());
    CHECK_EQ(try_catch.Exception().As<v8::Int32>()->Value(), 42);
  }
}

TEST(AtomicsWaitCallback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  const char* init = R"(
      let sab = new SharedArrayBuffer(16);
      let int32arr = new Int32Array(sab, 4);
      let setArrayElemAs = function(id, val) {
        int32arr[id] = val;
      };
      let wait = function(id, val, timeout) {
        if(arguments.length == 2) return Atomics.wait(int32arr, id, val);
        return Atomics.wait(int32arr, id, val, timeout);
      };
      sab;)";
  AtomicsWaitCallbackCommon(isolate, CompileRun(init), 4, 4);
}

namespace v8 {
namespace internal {
namespace wasm {
TEST(WasmI32AtomicWaitCallback) {
  FlagScope<bool> wasm_threads_flag(&i::FLAG_experimental_wasm_threads, true);
  WasmRunner<int32_t, int32_t, int32_t, double> r(ExecutionTier::kTurbofan);
  r.builder().AddMemory(kWasmPageSize, SharedFlag::kShared);
  r.builder().SetHasSharedMemory();
  BUILD(r, WASM_ATOMICS_WAIT(kExprI32AtomicWait, WASM_GET_LOCAL(0),
                             WASM_GET_LOCAL(1),
                             WASM_I64_SCONVERT_F64(WASM_GET_LOCAL(2)), 4));
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  Handle<JSFunction> func = r.builder().WrapCode(0);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("func"), v8::Utils::ToLocal(func))
            .FromJust());
  Handle<JSArrayBuffer> memory(
      r.builder().instance_object()->memory_object().array_buffer(), i_isolate);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("sab"), v8::Utils::ToLocal(memory))
            .FromJust());

  const char* init = R"(
      let int32arr = new Int32Array(sab, 4);
      let setArrayElemAs = function(id, val) {
        int32arr[id] = val;
      };
      let wait = function(id, val, timeout) {
        if(arguments.length === 2)
          return func(id << 2, val, -1);
        return func(id << 2, val, timeout*1000000);
      };
      sab;)";
  AtomicsWaitCallbackCommon(isolate, CompileRun(init), 4, 4);
}

TEST(WasmI64AtomicWaitCallback) {
  FlagScope<bool> wasm_threads_flag(&i::FLAG_experimental_wasm_threads, true);
  WasmRunner<int32_t, int32_t, double, double> r(ExecutionTier::kTurbofan);
  r.builder().AddMemory(kWasmPageSize, SharedFlag::kShared);
  r.builder().SetHasSharedMemory();
  BUILD(r, WASM_ATOMICS_WAIT(kExprI64AtomicWait, WASM_GET_LOCAL(0),
                             WASM_I64_SCONVERT_F64(WASM_GET_LOCAL(1)),
                             WASM_I64_SCONVERT_F64(WASM_GET_LOCAL(2)), 8));
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  Handle<JSFunction> func = r.builder().WrapCode(0);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("func"), v8::Utils::ToLocal(func))
            .FromJust());
  Handle<JSArrayBuffer> memory(
      r.builder().instance_object()->memory_object().array_buffer(), i_isolate);
  CHECK(env->Global()
            ->Set(env.local(), v8_str("sab"), v8::Utils::ToLocal(memory))
            .FromJust());

  const char* init = R"(
      let int64arr = new BigInt64Array(sab, 8);
      let setArrayElemAs = function(id, val) {
        int64arr[id] = BigInt(val);
      };
      let wait = function(id, val, timeout) {
        if(arguments.length === 2)
          return func(id << 3, val, -1);
        return func(id << 3, val, timeout*1000000);
      };
      sab;)";
  AtomicsWaitCallbackCommon(isolate, CompileRun(init), 8, 8);
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

// TODO(mstarzinger): Move this into a test-api-wasm.cc file when this large
// test file is being split up into chunks.
TEST(WasmModuleObjectCompileFailure) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    v8::TryCatch try_catch(isolate);
    uint8_t buffer[] = {0xDE, 0xAD, 0xBE, 0xEF};
    v8::MemorySpan<const uint8_t> serialized_module;
    v8::MemorySpan<const uint8_t> wire_bytes = {buffer, arraysize(buffer)};
    v8::MaybeLocal<v8::WasmModuleObject> maybe_module =
        v8::WasmModuleObject::DeserializeOrCompile(isolate, serialized_module,
                                                   wire_bytes);
    CHECK(maybe_module.IsEmpty());
    CHECK(try_catch.HasCaught());
  }
}

TEST(BigIntAPI) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  bool lossless;
  uint64_t words1[10];
  uint64_t words2[10];

  {
    Local<Value> bi = CompileRun("12n");
    CHECK(bi->IsBigInt());

    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(), 12);
    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(&lossless), 12);
    CHECK_EQ(lossless, true);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(), 12);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(&lossless), 12);
    CHECK_EQ(lossless, true);
  }

  {
    Local<Value> bi = CompileRun("-12n");
    CHECK(bi->IsBigInt());

    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(), static_cast<uint64_t>(-12));
    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(&lossless),
             static_cast<uint64_t>(-12));
    CHECK_EQ(lossless, false);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(), -12);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(&lossless), -12);
    CHECK_EQ(lossless, true);
  }

  {
    Local<Value> bi = CompileRun("123456789012345678901234567890n");
    CHECK(bi->IsBigInt());

    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(), 14083847773837265618ULL);
    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(&lossless),
             14083847773837265618ULL);
    CHECK_EQ(lossless, false);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(), -4362896299872285998LL);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(&lossless),
             -4362896299872285998LL);
    CHECK_EQ(lossless, false);
  }

  {
    Local<Value> bi = CompileRun("-123456789012345678901234567890n");
    CHECK(bi->IsBigInt());

    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(), 4362896299872285998LL);
    CHECK_EQ(bi.As<v8::BigInt>()->Uint64Value(&lossless),
             4362896299872285998LL);
    CHECK_EQ(lossless, false);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(), 4362896299872285998LL);
    CHECK_EQ(bi.As<v8::BigInt>()->Int64Value(&lossless), 4362896299872285998LL);
    CHECK_EQ(lossless, false);
  }

  {
    Local<v8::BigInt> bi =
        v8::BigInt::NewFromWords(env.local(), 0, 0, words1).ToLocalChecked();
    CHECK_EQ(bi->Uint64Value(), 0);
    CHECK_EQ(bi->WordCount(), 0);
  }

  {
    TryCatch try_catch(isolate);
    v8::MaybeLocal<v8::BigInt> bi = v8::BigInt::NewFromWords(
        env.local(), 0, std::numeric_limits<int>::max(), words1);
    CHECK(bi.IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  {
    TryCatch try_catch(isolate);
    v8::MaybeLocal<v8::BigInt> bi =
        v8::BigInt::NewFromWords(env.local(), 0, -1, words1);
    CHECK(bi.IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  {
    TryCatch try_catch(isolate);
    v8::MaybeLocal<v8::BigInt> bi =
        v8::BigInt::NewFromWords(env.local(), 0, 1 << 30, words1);
    CHECK(bi.IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  for (int sign_bit = 0; sign_bit <= 1; sign_bit++) {
    words1[0] = 0xffffffff00000000ULL;
    words1[1] = 0x00000000ffffffffULL;
    v8::Local<v8::BigInt> bi =
        v8::BigInt::NewFromWords(env.local(), sign_bit, 2, words1)
            .ToLocalChecked();
    CHECK_EQ(bi->Uint64Value(&lossless),
             sign_bit ? static_cast<uint64_t>(-static_cast<int64_t>(words1[0]))
                      : words1[0]);
    CHECK_EQ(lossless, false);
    CHECK_EQ(bi->Int64Value(&lossless), sign_bit
                                            ? -static_cast<int64_t>(words1[0])
                                            : static_cast<int64_t>(words1[0]));
    CHECK_EQ(lossless, false);
    CHECK_EQ(bi->WordCount(), 2);
    int real_sign_bit;
    int word_count = arraysize(words2);
    bi->ToWordsArray(&real_sign_bit, &word_count, words2);
    CHECK_EQ(real_sign_bit, sign_bit);
    CHECK_EQ(word_count, 2);
  }
}

namespace {

bool wasm_threads_enabled_value = false;

bool MockWasmThreadsEnabledCallback(Local<Context>) {
  return wasm_threads_enabled_value;
}

}  // namespace

TEST(TestSetWasmThreadsEnabledCallback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  v8::HandleScope scope(isolate);
  v8::Local<Context> context = Context::New(CcTest::isolate());
  i::Handle<i::Context> i_context = v8::Utils::OpenHandle(*context);

  // {Isolate::AreWasmThreadsEnabled} calls the callback set by the embedder if
  // such a callback exists. Otherwise it returns
  // {FLAG_experimental_wasm_threads}. First we test that the flag is returned
  // correctly if no callback is set. Then we test that the flag is ignored if
  // the callback is set.

  i::FLAG_experimental_wasm_threads = false;
  CHECK(!i_isolate->AreWasmThreadsEnabled(i_context));

  i::FLAG_experimental_wasm_threads = true;
  CHECK(i_isolate->AreWasmThreadsEnabled(i_context));

  isolate->SetWasmThreadsEnabledCallback(MockWasmThreadsEnabledCallback);
  wasm_threads_enabled_value = false;
  CHECK(!i_isolate->AreWasmThreadsEnabled(i_context));

  wasm_threads_enabled_value = true;
  i::FLAG_experimental_wasm_threads = false;
  CHECK(i_isolate->AreWasmThreadsEnabled(i_context));
}

TEST(TestGetUnwindState) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);

  v8::UnwindState unwind_state = isolate->GetUnwindState();
  v8::MemoryRange builtins_range = unwind_state.embedded_code_range;

  // Check that each off-heap builtin is within the builtins code range.
  if (i::FLAG_embedded_builtins) {
    for (int id = 0; id < i::Builtins::builtin_count; id++) {
      if (!i::Builtins::IsIsolateIndependent(id)) continue;
      i::Code builtin = i_isolate->builtins()->builtin(id);
      i::Address start = builtin.InstructionStart();
      i::Address end = start + builtin.InstructionSize();

      i::Address builtins_start =
          reinterpret_cast<i::Address>(builtins_range.start);
      CHECK(start >= builtins_start &&
            end < builtins_start + builtins_range.length_in_bytes);
    }
  } else {
    CHECK_EQ(nullptr, builtins_range.start);
    CHECK_EQ(0, builtins_range.length_in_bytes);
  }

  v8::JSEntryStub js_entry_stub = unwind_state.js_entry_stub;

  CHECK_EQ(i_isolate->heap()->builtin(i::Builtins::kJSEntry).InstructionStart(),
           reinterpret_cast<i::Address>(js_entry_stub.code.start));
}

TEST(MicrotaskContextShouldBeNativeContext) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope scope(isolate);
    i::Handle<i::Context> context =
        v8::Utils::OpenHandle(*isolate->GetEnteredOrMicrotaskContext());

    CHECK(context->IsNativeContext());
    info.GetReturnValue().SetUndefined();
  };

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->SetCallAsFunctionHandler(callback);
  Local<v8::Object> obj = desc->GetFunction(env.local())
                              .ToLocalChecked()
                              ->NewInstance(env.local())
                              .ToLocalChecked();

  CHECK(env->Global()->Set(env.local(), v8_str("callback"), obj).FromJust());
  CompileRun(
      "with({}){(async ()=>{"
      "  await 42;"
      "})().then(callback);}");

  isolate->RunMicrotasks();
}

TEST(PreviewSetKeysIteratorEntriesWithDeleted) {
  LocalContext env;
  v8::HandleScope handle_scope(env->GetIsolate());
  v8::Local<v8::Context> context = env.local();

  {
    // Create set, delete entry, create iterator, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); set.delete(1); set.keys()")
            ->ToObject(context)
            .ToLocalChecked();
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); set.keys()")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, iterate, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.keys(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(1, entries->Length());
    CHECK_EQ(3, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, iterate until empty, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.keys(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next(); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(0, entries->Length());
  }
  {
    // Create set, create iterator, delete entry, iterate, trigger rehash,
    // preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.keys(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next();");
    CompileRun("for (var i = 4; i < 20; i++) set.add(i);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(17, entries->Length());
    for (uint32_t i = 0; i < 17; i++) {
      CHECK_EQ(i + 3, entries->Get(context, i)
                          .ToLocalChecked()
                          ->Int32Value(context)
                          .FromJust());
    }
  }
}

TEST(PreviewSetValuesIteratorEntriesWithDeleted) {
  LocalContext env;
  v8::HandleScope handle_scope(env->GetIsolate());
  v8::Local<v8::Context> context = env.local();

  {
    // Create set, delete entry, create iterator, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); set.delete(1); set.values()")
            ->ToObject(context)
            .ToLocalChecked();
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); set.values()")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, iterate, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.values(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(1, entries->Length());
    CHECK_EQ(3, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create set, create iterator, delete entry, iterate until empty, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.values(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next(); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(0, entries->Length());
  }
  {
    // Create set, create iterator, delete entry, iterate, trigger rehash,
    // preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); var it = set.values(); it")
            ->ToObject(context)
            .ToLocalChecked();
    CompileRun("set.delete(1); it.next();");
    CompileRun("for (var i = 4; i < 20; i++) set.add(i);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(17, entries->Length());
    for (uint32_t i = 0; i < 17; i++) {
      CHECK_EQ(i + 3, entries->Get(context, i)
                          .ToLocalChecked()
                          ->Int32Value(context)
                          .FromJust());
    }
  }
}

TEST(PreviewMapEntriesIteratorEntries) {
  LocalContext env;
  v8::HandleScope handle_scope(env->GetIsolate());
  v8::Local<v8::Context> context = env.local();
  {
    // Create set, delete entry, create entries iterator, preview.
    v8::Local<v8::Object> iterator =
        CompileRun("var set = new Set([1,2,3]); set.delete(2); set.entries()")
            ->ToObject(context)
            .ToLocalChecked();
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(is_key);
    CHECK_EQ(4, entries->Length());
    uint32_t first = entries->Get(context, 0)
                         .ToLocalChecked()
                         ->Int32Value(context)
                         .FromJust();
    uint32_t second = entries->Get(context, 2)
                          .ToLocalChecked()
                          ->Int32Value(context)
                          .FromJust();
    CHECK_EQ(1, first);
    CHECK_EQ(3, second);
    CHECK_EQ(first, entries->Get(context, 1)
                        .ToLocalChecked()
                        ->Int32Value(context)
                        .FromJust());
    CHECK_EQ(second, entries->Get(context, 3)
                         .ToLocalChecked()
                         ->Int32Value(context)
                         .FromJust());
  }
}

TEST(PreviewMapValuesIteratorEntriesWithDeleted) {
  LocalContext env;
  v8::HandleScope handle_scope(env->GetIsolate());
  v8::Local<v8::Context> context = env.local();

  {
    // Create map, delete entry, create iterator, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = {}; map.set(key, 1);"
                                         "map.set({}, 2); map.set({}, 3);"
                                         "map.delete(key);"
                                         "map.values()")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = {}; map.set(key, 1);"
                                         "map.set({}, 2); map.set({}, 3);"
                                         "map.values()")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, iterate, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = {}; map.set(key, 1);"
                                         "map.set({}, 2); map.set({}, 3);"
                                         "var it = map.values(); it")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(1, entries->Length());
    CHECK_EQ(3, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, iterate until empty, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = {}; map.set(key, 1);"
                                         "map.set({}, 2); map.set({}, 3);"
                                         "var it = map.values(); it")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key); it.next(); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(0, entries->Length());
  }
  {
    // Create map, create iterator, delete entry, iterate, trigger rehash,
    // preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = {}; map.set(key, 1);"
                                         "map.set({}, 2); map.set({}, 3);"
                                         "var it = map.values(); it")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key); it.next();");
    CompileRun("for (var i = 4; i < 20; i++) map.set({}, i);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(17, entries->Length());
    for (uint32_t i = 0; i < 17; i++) {
      CHECK_EQ(i + 3, entries->Get(context, i)
                          .ToLocalChecked()
                          ->Int32Value(context)
                          .FromJust());
    }
  }
}

TEST(PreviewMapKeysIteratorEntriesWithDeleted) {
  LocalContext env;
  v8::HandleScope handle_scope(env->GetIsolate());
  v8::Local<v8::Context> context = env.local();

  {
    // Create map, delete entry, create iterator, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = 1; map.set(key, {});"
                                         "map.set(2, {}); map.set(3, {});"
                                         "map.delete(key);"
                                         "map.keys()")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = 1; map.set(key, {});"
                                         "map.set(2, {}); map.set(3, {});"
                                         "map.keys()")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key);");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(2, entries->Length());
    CHECK_EQ(2, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
    CHECK_EQ(3, entries->Get(context, 1)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, iterate, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = 1; map.set(key, {});"
                                         "map.set(2, {}); map.set(3, {});"
                                         "var it = map.keys(); it")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(1, entries->Length());
    CHECK_EQ(3, entries->Get(context, 0)
                    .ToLocalChecked()
                    ->Int32Value(context)
                    .FromJust());
  }
  {
    // Create map, create iterator, delete entry, iterate until empty, preview.
    v8::Local<v8::Object> iterator = CompileRun(
                                         "var map = new Map();"
                                         "var key = 1; map.set(key, {});"
                                         "map.set(2, {}); map.set(3, {});"
                                         "var it = map.keys(); it")
                                         ->ToObject(context)
                                         .ToLocalChecked();
    CompileRun("map.delete(key); it.next(); it.next();");
    bool is_key;
    v8::Local<v8::Array> entries =
        iterator->PreviewEntries(&is_key).ToLocalChecked();
    CHECK(!is_key);
    CHECK_EQ(0, entries->Length());
  }
}

namespace {
static v8::Isolate* isolate_1;
static v8::Isolate* isolate_2;
v8::Persistent<v8::Context> context_1;
v8::Persistent<v8::Context> context_2;

static void CallIsolate1(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate::Scope isolate_scope(isolate_1);
  v8::HandleScope handle_scope(isolate_1);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate_1, context_1);
  v8::Context::Scope context_scope(context);
  CompileRun("f1() //# sourceURL=isolate1b");
}

static void CallIsolate2(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate::Scope isolate_scope(isolate_2);
  v8::HandleScope handle_scope(isolate_2);
  v8::Local<v8::Context> context =
      v8::Local<v8::Context>::New(isolate_2, context_2);
  v8::Context::Scope context_scope(context);
  reinterpret_cast<i::Isolate*>(isolate_2)->heap()->CollectAllGarbage(
      i::Heap::kNoGCFlags, i::GarbageCollectionReason::kTesting,
      v8::kGCCallbackFlagForced);
  CompileRun("f2() //# sourceURL=isolate2b");
}

}  // anonymous namespace

UNINITIALIZED_TEST(NestedIsolates) {
#ifdef VERIFY_HEAP
  i::FLAG_verify_heap = true;
#endif  // VERIFY_HEAP
  // Create two isolates and set up C++ functions via function templates that
  // call into the other isolate. Recurse a few times, trigger GC along the way,
  // and finally capture a stack trace. Check that the stack trace only includes
  // frames from its own isolate.
  i::FLAG_stack_trace_limit = 20;
  i::FLAG_experimental_stack_trace_frames = true;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  isolate_1 = v8::Isolate::New(create_params);
  isolate_2 = v8::Isolate::New(create_params);

  {
    v8::Isolate::Scope isolate_scope(isolate_1);
    v8::HandleScope handle_scope(isolate_1);

    v8::Local<v8::Context> context = v8::Context::New(isolate_1);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate_1, CallIsolate2);
    fun_templ->SetClassName(v8_str(isolate_1, "call_isolate_2"));
    Local<Function> fun = fun_templ->GetFunction(context).ToLocalChecked();
    CHECK(context->Global()
              ->Set(context, v8_str(isolate_1, "call_isolate_2"), fun)
              .FromJust());
    CompileRun(
        "let c = 0;"
        "function f1() {"
        "  c++;"
        "  return call_isolate_2();"
        "} //# sourceURL=isolate1a");
    context_1.Reset(isolate_1, context);
  }

  {
    v8::Isolate::Scope isolate_scope(isolate_2);
    v8::HandleScope handle_scope(isolate_2);

    v8::Local<v8::Context> context = v8::Context::New(isolate_2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate_2, CallIsolate1);
    fun_templ->SetClassName(v8_str(isolate_2, "call_isolate_1"));
    Local<Function> fun = fun_templ->GetFunction(context).ToLocalChecked();

    CHECK(context->Global()
              ->Set(context, v8_str(isolate_2, "call_isolate_1"), fun)
              .FromJust());
    CompileRun(
        "let c = 4;"
        "let result = undefined;"
        "function f2() {"
        "  if (c-- > 0) return call_isolate_1();"
        "  else result = new Error().stack;"
        "} //# sourceURL=isolate2a");
    context_2.Reset(isolate_2, context);

    v8::Local<v8::String> result =
        CompileRun("f2(); result //# sourceURL=isolate2c")
            ->ToString(context)
            .ToLocalChecked();
    v8::Local<v8::String> expectation =
        v8_str(isolate_2,
               "Error\n"
               "    at f2 (isolate2a:1:104)\n"
               "    at isolate2b:1:1\n"
               "    at call_isolate_1 (<anonymous>)\n"
               "    at f2 (isolate2a:1:71)\n"
               "    at isolate2b:1:1\n"
               "    at call_isolate_1 (<anonymous>)\n"
               "    at f2 (isolate2a:1:71)\n"
               "    at isolate2b:1:1\n"
               "    at call_isolate_1 (<anonymous>)\n"
               "    at f2 (isolate2a:1:71)\n"
               "    at isolate2b:1:1\n"
               "    at call_isolate_1 (<anonymous>)\n"
               "    at f2 (isolate2a:1:71)\n"
               "    at isolate2c:1:1");
    CHECK(result->StrictEquals(expectation));
  }

  {
    v8::Isolate::Scope isolate_scope(isolate_1);
    v8::HandleScope handle_scope(isolate_1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate_1, context_1);
    v8::Context::Scope context_scope(context);
    ExpectInt32("c", 4);
  }

  isolate_1->Dispose();
  isolate_2->Dispose();
}

#undef THREADED_PROFILED_TEST
