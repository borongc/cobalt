// Copyright 2007-2010 the V8 project authors. All rights reserved.
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

#include <signal.h>
#include <sys/stat.h>

#include "include/v8-extension.h"
#include "include/v8-function.h"
#include "include/v8-locker.h"
#include "src/api/api-inl.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/codegen/script-details.h"
#include "src/common/assert-scope.h"
#include "src/heap/heap-inl.h"
#include "src/heap/parked-scope.h"
#include "src/heap/read-only-heap.h"
#include "src/heap/safepoint.h"
#include "src/heap/spaces.h"
#include "src/numbers/hash-seed-inl.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-regexp-inl.h"
#include "src/objects/objects-inl.h"
#include "src/runtime/runtime.h"
#include "src/snapshot/code-serializer.h"
#include "src/snapshot/context-deserializer.h"
#include "src/snapshot/context-serializer.h"
#include "src/snapshot/read-only-deserializer.h"
#include "src/snapshot/read-only-serializer.h"
#include "src/snapshot/shared-heap-deserializer.h"
#include "src/snapshot/shared-heap-serializer.h"
#include "src/snapshot/snapshot-compression.h"
#include "src/snapshot/snapshot.h"
#include "src/snapshot/startup-deserializer.h"
#include "src/snapshot/startup-serializer.h"
#include "test/cctest/cctest.h"
#include "test/cctest/heap/heap-utils.h"
#include "test/cctest/setup-isolate-for-tests.h"

namespace v8 {
namespace internal {

enum CodeCacheType { kLazy, kEager, kAfterExecute };

void DisableAlwaysOpt() {
  // Isolates prepared for serialization do not optimize. The only exception is
  // with the flag --always-turbofan.
  v8_flags.always_turbofan = false;
}

// A convenience struct to simplify management of the blobs required to
// deserialize an isolate.
struct StartupBlobs {
  base::Vector<const byte> startup;
  base::Vector<const byte> read_only;
  base::Vector<const byte> shared_space;

  void Dispose() {
    startup.Dispose();
    read_only.Dispose();
    shared_space.Dispose();
  }
};

// TestSerializer is used for testing isolate serialization.
class TestSerializer {
 public:
  static v8::Isolate* NewIsolateInitialized() {
    const bool kEnableSerializer = true;
    DisableEmbeddedBlobRefcounting();
    v8::Isolate* v8_isolate = NewIsolate(kEnableSerializer);
    v8::Isolate::Scope isolate_scope(v8_isolate);
    i::Isolate* isolate = reinterpret_cast<i::Isolate*>(v8_isolate);
    isolate->InitWithoutSnapshot();
    return v8_isolate;
  }

  // Wraps v8::Isolate::New, but with a test isolate under the hood.
  // Allows flexibility to bootstrap with or without snapshot even when
  // the production Isolate class has one or the other behavior baked in.
  static v8::Isolate* NewIsolate(const v8::Isolate::CreateParams& params) {
    const bool kEnableSerializer = false;
    v8::Isolate* v8_isolate = NewIsolate(kEnableSerializer);
    v8::Isolate::Initialize(v8_isolate, params);
    return v8_isolate;
  }

  static v8::Isolate* NewIsolateFromBlob(const StartupBlobs& blobs) {
    SnapshotData startup_snapshot(blobs.startup);
    SnapshotData read_only_snapshot(blobs.read_only);
    SnapshotData shared_space_snapshot(blobs.shared_space);
    const bool kEnableSerializer = false;
    v8::Isolate* v8_isolate = NewIsolate(kEnableSerializer);
    v8::Isolate::Scope isolate_scope(v8_isolate);
    i::Isolate* isolate = reinterpret_cast<i::Isolate*>(v8_isolate);
    isolate->InitWithSnapshot(&startup_snapshot, &read_only_snapshot,
                              &shared_space_snapshot, false);
    return v8_isolate;
  }

 private:
  // Creates an Isolate instance configured for testing.
  static v8::Isolate* NewIsolate(bool with_serializer) {
    i::Isolate* isolate = i::Isolate::New();
    v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);

    if (with_serializer) isolate->enable_serializer();
    isolate->set_array_buffer_allocator(CcTest::array_buffer_allocator());
    isolate->setup_delegate_ = new SetupIsolateDelegateForTests;

    return v8_isolate;
  }
};

static base::Vector<const byte> WritePayload(
    const base::Vector<const byte>& payload) {
  int length = payload.length();
  byte* blob = NewArray<byte>(length);
  memcpy(blob, payload.begin(), length);
  return base::Vector<const byte>(const_cast<const byte*>(blob), length);
}

namespace {

// Convenience wrapper around the convenience wrapper.
v8::StartupData CreateSnapshotDataBlob(const char* embedded_source) {
  v8::StartupData data = CreateSnapshotDataBlobInternal(
      v8::SnapshotCreator::FunctionCodeHandling::kClear, embedded_source);
  return data;
}

}  // namespace

static StartupBlobs Serialize(v8::Isolate* isolate) {
  // We have to create one context.  One reason for this is so that the builtins
  // can be loaded from self hosted JS builtins and their addresses can be
  // processed.  This will clear the pending fixups array, which would otherwise
  // contain GC roots that would confuse the serialization/deserialization
  // process.
  v8::Isolate::Scope isolate_scope(isolate);
  {
    v8::HandleScope scope(isolate);
    v8::Context::New(isolate);
  }

  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);
  CcTest::CollectAllAvailableGarbage(i_isolate);

  IsolateSafepointScope safepoint(i_isolate->heap());
  HandleScope scope(i_isolate);

  DisallowGarbageCollection no_gc;
  ReadOnlySerializer read_only_serializer(i_isolate,
                                          Snapshot::kDefaultSerializerFlags);
  read_only_serializer.SerializeReadOnlyRoots();

  SharedHeapSerializer shared_space_serializer(
      i_isolate, Snapshot::kDefaultSerializerFlags, &read_only_serializer);

  StartupSerializer ser(i_isolate, Snapshot::kDefaultSerializerFlags,
                        &read_only_serializer, &shared_space_serializer);
  ser.SerializeStrongReferences(no_gc);

  ser.SerializeWeakReferencesAndDeferred();

  shared_space_serializer.FinalizeSerialization();
  read_only_serializer.FinalizeSerialization();
  SnapshotData startup_snapshot(&ser);
  SnapshotData read_only_snapshot(&read_only_serializer);
  SnapshotData shared_space_snapshot(&shared_space_serializer);
  return {WritePayload(startup_snapshot.RawData()),
          WritePayload(read_only_snapshot.RawData()),
          WritePayload(shared_space_snapshot.RawData())};
}

base::Vector<const char> ConstructSource(base::Vector<const char> head,
                                         base::Vector<const char> body,
                                         base::Vector<const char> tail,
                                         int repeats) {
  size_t source_length = head.size() + body.size() * repeats + tail.size();
  char* source = NewArray<char>(source_length);
  CopyChars(source, head.begin(), head.length());
  for (int i = 0; i < repeats; i++) {
    CopyChars(source + head.length() + i * body.length(), body.begin(),
              body.length());
  }
  CopyChars(source + head.length() + repeats * body.length(), tail.begin(),
            tail.length());
  return base::VectorOf(source, source_length);
}

static v8::Isolate* Deserialize(const StartupBlobs& blobs) {
  v8::Isolate* isolate = TestSerializer::NewIsolateFromBlob(blobs);
  CHECK(isolate);
  return isolate;
}

static void SanityCheck(v8::Isolate* v8_isolate) {
  Isolate* isolate = reinterpret_cast<Isolate*>(v8_isolate);
  v8::HandleScope scope(v8_isolate);
#ifdef VERIFY_HEAP
  HeapVerifier::VerifyHeap(isolate->heap());
#endif
  CHECK(isolate->global_object()->IsJSObject());
  CHECK(isolate->native_context()->IsContext());
  isolate->factory()->InternalizeString(base::StaticCharVector("Empty"));
}

void TestStartupSerializerOnceImpl() {
  v8::Isolate* isolate = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs = Serialize(isolate);
  isolate->Dispose();
  isolate = Deserialize(blobs);
  {
    v8::HandleScope handle_scope(isolate);
    v8::Isolate::Scope isolate_scope(isolate);

    v8::Local<v8::Context> env = v8::Context::New(isolate);
    env->Enter();

    SanityCheck(isolate);
  }
  isolate->Dispose();
  blobs.Dispose();
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(StartupSerializerOnce) {
  DisableAlwaysOpt();
  TestStartupSerializerOnceImpl();
}

UNINITIALIZED_TEST(StartupSerializerTwice) {
  DisableAlwaysOpt();
  v8::Isolate* isolate = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs1 = Serialize(isolate);
  StartupBlobs blobs2 = Serialize(isolate);
  isolate->Dispose();
  blobs1.Dispose();
  isolate = Deserialize(blobs2);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    v8::Local<v8::Context> env = v8::Context::New(isolate);
    env->Enter();

    SanityCheck(isolate);
  }
  isolate->Dispose();
  blobs2.Dispose();
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(StartupSerializerOnceRunScript) {
  DisableAlwaysOpt();
  v8::Isolate* isolate = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs = Serialize(isolate);
  isolate->Dispose();
  isolate = Deserialize(blobs);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    v8::Local<v8::Context> env = v8::Context::New(isolate);
    env->Enter();

    const char* c_source = "\"1234\".length";
    v8::Local<v8::Script> script = v8_compile(c_source);
    v8::Maybe<int32_t> result = script->Run(isolate->GetCurrentContext())
                                    .ToLocalChecked()
                                    ->Int32Value(isolate->GetCurrentContext());
    CHECK_EQ(4, result.FromJust());
  }
  isolate->Dispose();
  blobs.Dispose();
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(StartupSerializerTwiceRunScript) {
  DisableAlwaysOpt();
  v8::Isolate* isolate = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs1 = Serialize(isolate);
  StartupBlobs blobs2 = Serialize(isolate);
  isolate->Dispose();
  blobs1.Dispose();
  isolate = Deserialize(blobs2);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    v8::Local<v8::Context> env = v8::Context::New(isolate);
    env->Enter();

    const char* c_source = "\"1234\".length";
    v8::Local<v8::Script> script = v8_compile(c_source);
    v8::Maybe<int32_t> result = script->Run(isolate->GetCurrentContext())
                                    .ToLocalChecked()
                                    ->Int32Value(isolate->GetCurrentContext());
    CHECK_EQ(4, result.FromJust());
  }
  isolate->Dispose();
  blobs2.Dispose();
  FreeCurrentEmbeddedBlob();
}

static void SerializeContext(base::Vector<const byte>* startup_blob_out,
                             base::Vector<const byte>* read_only_blob_out,
                             base::Vector<const byte>* shared_space_blob_out,
                             base::Vector<const byte>* context_blob_out) {
  v8::Isolate* v8_isolate = TestSerializer::NewIsolateInitialized();
  Isolate* isolate = reinterpret_cast<Isolate*>(v8_isolate);
  Heap* heap = isolate->heap();
  {
    v8::Isolate::Scope isolate_scope(v8_isolate);

    v8::Persistent<v8::Context> env;
    {
      HandleScope scope(isolate);
      env.Reset(v8_isolate, v8::Context::New(v8_isolate));
    }
    CHECK(!env.IsEmpty());
    {
      v8::HandleScope handle_scope(v8_isolate);
      v8::Local<v8::Context>::New(v8_isolate, env)->Enter();
    }

    // If we don't do this then we end up with a stray root pointing at the
    // context even after we have disposed of env.
    CcTest::CollectAllAvailableGarbage(isolate);

    {
      v8::HandleScope handle_scope(v8_isolate);
      v8::Local<v8::Context>::New(v8_isolate, env)->Exit();
    }

    HandleScope scope(isolate);
    i::Context raw_context = i::Context::cast(*v8::Utils::OpenPersistent(env));

    env.Reset();

    IsolateSafepointScope safepoint(heap);

    DisallowGarbageCollection no_gc;
    SnapshotByteSink read_only_sink;
    ReadOnlySerializer read_only_serializer(isolate,
                                            Snapshot::kDefaultSerializerFlags);
    read_only_serializer.SerializeReadOnlyRoots();

    SharedHeapSerializer shared_space_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &read_only_serializer);

    SnapshotByteSink startup_sink;
    StartupSerializer startup_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &read_only_serializer,
        &shared_space_serializer);
    startup_serializer.SerializeStrongReferences(no_gc);

    SnapshotByteSink context_sink;
    ContextSerializer context_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &startup_serializer,
        v8::SerializeInternalFieldsCallback());
    context_serializer.Serialize(&raw_context, no_gc);

    startup_serializer.SerializeWeakReferencesAndDeferred();

    shared_space_serializer.FinalizeSerialization();
    read_only_serializer.FinalizeSerialization();

    SnapshotData read_only_snapshot(&read_only_serializer);
    SnapshotData shared_space_snapshot(&shared_space_serializer);
    SnapshotData startup_snapshot(&startup_serializer);
    SnapshotData context_snapshot(&context_serializer);

    *context_blob_out = WritePayload(context_snapshot.RawData());
    *startup_blob_out = WritePayload(startup_snapshot.RawData());
    *read_only_blob_out = WritePayload(read_only_snapshot.RawData());
    *shared_space_blob_out = WritePayload(shared_space_snapshot.RawData());
  }
  v8_isolate->Dispose();
}

#ifdef SNAPSHOT_COMPRESSION
UNINITIALIZED_TEST(SnapshotCompression) {
  DisableAlwaysOpt();
  base::Vector<const byte> startup_blob;
  base::Vector<const byte> read_only_blob;
  base::Vector<const byte> shared_space_blob;
  base::Vector<const byte> context_blob;
  SerializeContext(&startup_blob, &read_only_blob, &shared_space_blob,
                   &context_blob);
  SnapshotData original_snapshot_data(context_blob);
  SnapshotData compressed =
      i::SnapshotCompression::Compress(&original_snapshot_data);
  SnapshotData decompressed =
      i::SnapshotCompression::Decompress(compressed.RawData());
  CHECK_EQ(context_blob, decompressed.RawData());

  startup_blob.Dispose();
  read_only_blob.Dispose();
  shared_space_blob.Dispose();
  context_blob.Dispose();
}
#endif  // SNAPSHOT_COMPRESSION

UNINITIALIZED_TEST(ContextSerializerContext) {
  DisableAlwaysOpt();
  base::Vector<const byte> startup_blob;
  base::Vector<const byte> read_only_blob;
  base::Vector<const byte> shared_space_blob;
  base::Vector<const byte> context_blob;
  SerializeContext(&startup_blob, &read_only_blob, &shared_space_blob,
                   &context_blob);

  StartupBlobs blobs = {startup_blob, read_only_blob, shared_space_blob};
  v8::Isolate* v8_isolate = TestSerializer::NewIsolateFromBlob(blobs);
  CHECK(v8_isolate);
  {
    v8::Isolate::Scope isolate_scope(v8_isolate);

    Isolate* isolate = reinterpret_cast<Isolate*>(v8_isolate);
    HandleScope handle_scope(isolate);
    Handle<Object> root;
    Handle<JSGlobalProxy> global_proxy =
        isolate->factory()->NewUninitializedJSGlobalProxy(
            JSGlobalProxy::SizeWithEmbedderFields(0));
    {
      SnapshotData snapshot_data(context_blob);
      root = ContextDeserializer::DeserializeContext(
                 isolate, &snapshot_data, false, global_proxy,
                 v8::DeserializeInternalFieldsCallback())
                 .ToHandleChecked();
      CHECK(root->IsContext());
      CHECK(Handle<Context>::cast(root)->global_proxy() == *global_proxy);
    }

    Handle<Object> root2;
    {
      SnapshotData snapshot_data(context_blob);
      root2 = ContextDeserializer::DeserializeContext(
                  isolate, &snapshot_data, false, global_proxy,
                  v8::DeserializeInternalFieldsCallback())
                  .ToHandleChecked();
      CHECK(root2->IsContext());
      CHECK(!root.is_identical_to(root2));
    }
    context_blob.Dispose();
  }
  v8_isolate->Dispose();
  blobs.Dispose();
  FreeCurrentEmbeddedBlob();
}

static void SerializeCustomContext(
    base::Vector<const byte>* startup_blob_out,
    base::Vector<const byte>* read_only_blob_out,
    base::Vector<const byte>* shared_space_blob_out,
    base::Vector<const byte>* context_blob_out) {
  v8::Isolate* v8_isolate = TestSerializer::NewIsolateInitialized();
  Isolate* isolate = reinterpret_cast<Isolate*>(v8_isolate);
  {
    v8::Isolate::Scope isolate_scope(v8_isolate);

    v8::Persistent<v8::Context> env;
    {
      HandleScope scope(isolate);
      env.Reset(v8_isolate, v8::Context::New(v8_isolate));
    }
    CHECK(!env.IsEmpty());
    {
      v8::HandleScope handle_scope(v8_isolate);
      v8::Local<v8::Context>::New(v8_isolate, env)->Enter();
      // After execution, e's function context refers to the global object.
      CompileRun(
          "var e;"
          "(function() {"
          "  e = function(s) { return eval (s); }"
          "})();"
          "var o = this;"
          "var r = Math.random();"
          "var c = Math.sin(0) + Math.cos(0);"
          "var f = (function(a, b) { return a + b; }).bind(1, 2, 3);"
          "var s = parseInt('12345');"
          "var p = 0;"
          "(async ()=>{ p = await 42; })();");

      base::Vector<const char> source = ConstructSource(
          base::StaticCharVector("function g() { return [,"),
          base::StaticCharVector("1,"),
          base::StaticCharVector("];} a = g(); b = g(); b.push(1);"), 100000);
      v8::MaybeLocal<v8::String> source_str =
          v8::String::NewFromUtf8(v8_isolate, source.begin(),
                                  v8::NewStringType::kNormal, source.length());
      CompileRun(source_str.ToLocalChecked());
      source.Dispose();
    }
    // If we don't do this then we end up with a stray root pointing at the
    // context even after we have disposed of env.
    CcTest::CollectAllAvailableGarbage(isolate);

    {
      v8::HandleScope handle_scope(v8_isolate);
      v8::Local<v8::Context>::New(v8_isolate, env)->Exit();
    }

    HandleScope scope(isolate);
    i::Context raw_context = i::Context::cast(*v8::Utils::OpenPersistent(env));

    env.Reset();

    IsolateSafepointScope safepoint(isolate->heap());

    DisallowGarbageCollection no_gc;
    SnapshotByteSink read_only_sink;
    ReadOnlySerializer read_only_serializer(isolate,
                                            Snapshot::kDefaultSerializerFlags);
    read_only_serializer.SerializeReadOnlyRoots();

    SharedHeapSerializer shared_space_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &read_only_serializer);

    SnapshotByteSink startup_sink;
    StartupSerializer startup_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &read_only_serializer,
        &shared_space_serializer);
    startup_serializer.SerializeStrongReferences(no_gc);

    SnapshotByteSink context_sink;
    ContextSerializer context_serializer(
        isolate, Snapshot::kDefaultSerializerFlags, &startup_serializer,
        v8::SerializeInternalFieldsCallback());
    context_serializer.Serialize(&raw_context, no_gc);

    startup_serializer.SerializeWeakReferencesAndDeferred();

    shared_space_serializer.FinalizeSerialization();
    read_only_serializer.FinalizeSerialization();

    SnapshotData read_only_snapshot(&read_only_serializer);
    SnapshotData shared_space_snapshot(&shared_space_serializer);
    SnapshotData startup_snapshot(&startup_serializer);
    SnapshotData context_snapshot(&context_serializer);

    *context_blob_out = WritePayload(context_snapshot.RawData());
    *startup_blob_out = WritePayload(startup_snapshot.RawData());
    *read_only_blob_out = WritePayload(read_only_snapshot.RawData());
    *shared_space_blob_out = WritePayload(shared_space_snapshot.RawData());
  }
  v8_isolate->Dispose();
}

UNINITIALIZED_TEST(ContextSerializerCustomContext) {
  DisableAlwaysOpt();
  base::Vector<const byte> startup_blob;
  base::Vector<const byte> read_only_blob;
  base::Vector<const byte> shared_space_blob;
  base::Vector<const byte> context_blob;
  SerializeCustomContext(&startup_blob, &read_only_blob, &shared_space_blob,
                         &context_blob);

  StartupBlobs blobs = {startup_blob, read_only_blob, shared_space_blob};
  v8::Isolate* v8_isolate = TestSerializer::NewIsolateFromBlob(blobs);
  CHECK(v8_isolate);
  {
    v8::Isolate::Scope isolate_scope(v8_isolate);

    Isolate* isolate = reinterpret_cast<Isolate*>(v8_isolate);
    HandleScope handle_scope(isolate);
    Handle<Object> root;
    Handle<JSGlobalProxy> global_proxy =
        isolate->factory()->NewUninitializedJSGlobalProxy(
            JSGlobalProxy::SizeWithEmbedderFields(0));
    {
      SnapshotData snapshot_data(context_blob);
      root = ContextDeserializer::DeserializeContext(
                 isolate, &snapshot_data, false, global_proxy,
                 v8::DeserializeInternalFieldsCallback())
                 .ToHandleChecked();
      CHECK(root->IsContext());
      Handle<Context> context = Handle<Context>::cast(root);

      // Add context to the weak native context list
      context->set(Context::NEXT_CONTEXT_LINK,
                   isolate->heap()->native_contexts_list(),
                   UPDATE_WRITE_BARRIER);
      isolate->heap()->set_native_contexts_list(*context);

      CHECK(context->global_proxy() == *global_proxy);
      Handle<String> o = isolate->factory()->NewStringFromAsciiChecked("o");
      Handle<JSObject> global_object(context->global_object(), isolate);
      Handle<Object> property =
          JSReceiver::GetDataProperty(isolate, global_object, o);
      CHECK(property.is_identical_to(global_proxy));

      v8::Local<v8::Context> v8_context = v8::Utils::ToLocal(context);
      v8::Context::Scope context_scope(v8_context);
      double r = CompileRun("r")
                     ->ToNumber(v8_isolate->GetCurrentContext())
                     .ToLocalChecked()
                     ->Value();
      CHECK(0.0 <= r && r < 1.0);
      // Math.random still works.
      double random = CompileRun("Math.random()")
                          ->ToNumber(v8_isolate->GetCurrentContext())
                          .ToLocalChecked()
                          ->Value();
      CHECK(0.0 <= random && random < 1.0);
      double c = CompileRun("c")
                     ->ToNumber(v8_isolate->GetCurrentContext())
                     .ToLocalChecked()
                     ->Value();
      CHECK_EQ(1, c);
      int f = CompileRun("f()")
                  ->ToNumber(v8_isolate->GetCurrentContext())
                  .ToLocalChecked()
                  ->Int32Value(v8_isolate->GetCurrentContext())
                  .FromJust();
      CHECK_EQ(5, f);
      f = CompileRun("e('f()')")
              ->ToNumber(v8_isolate->GetCurrentContext())
              .ToLocalChecked()
              ->Int32Value(v8_isolate->GetCurrentContext())
              .FromJust();
      CHECK_EQ(5, f);
      v8::Local<v8::String> s = CompileRun("s")
                                    ->ToString(v8_isolate->GetCurrentContext())
                                    .ToLocalChecked();
      CHECK(s->Equals(v8_isolate->GetCurrentContext(), v8_str("12345"))
                .FromJust());
      v8::Local<v8::String> p = CompileRun("p")
                                    ->ToString(v8_isolate->GetCurrentContext())
                                    .ToLocalChecked();
      CHECK(
          p->Equals(v8_isolate->GetCurrentContext(), v8_str("42")).FromJust());
      int a = CompileRun("a.length")
                  ->ToNumber(v8_isolate->GetCurrentContext())
                  .ToLocalChecked()
                  ->Int32Value(v8_isolate->GetCurrentContext())
                  .FromJust();
      CHECK_EQ(100001, a);
      int b = CompileRun("b.length")
                  ->ToNumber(v8_isolate->GetCurrentContext())
                  .ToLocalChecked()
                  ->Int32Value(v8_isolate->GetCurrentContext())
                  .FromJust();
      CHECK_EQ(100002, b);
    }
    context_blob.Dispose();
  }
  v8_isolate->Dispose();
  blobs.Dispose();
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlob1) {
  DisableAlwaysOpt();
  const char* source1 = "function f() { return 42; }";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate1 = TestSerializer::NewIsolate(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope c_scope(context);
    v8::Maybe<int32_t> result =
        CompileRun("f()")->Int32Value(isolate1->GetCurrentContext());
    CHECK_EQ(42, result.FromJust());
    CHECK(CompileRun("this.g")->IsUndefined());
  }
  isolate1->Dispose();
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

static void UnreachableCallback(const FunctionCallbackInfo<Value>& info) {
  UNREACHABLE();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobOverwriteGlobal) {
  DisableAlwaysOpt();
  const char* source1 = "function f() { return 42; }";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test that the snapshot overwrites the object template when there are
  // duplicate global properties.
  v8::Isolate* isolate1 = TestSerializer::NewIsolate(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::ObjectTemplate> global_template =
        v8::ObjectTemplate::New(isolate1);
    global_template->Set(
        isolate1, "f",
        v8::FunctionTemplate::New(isolate1, UnreachableCallback));
    v8::Local<v8::Context> context =
        v8::Context::New(isolate1, nullptr, global_template);
    v8::Context::Scope c_scope(context);
    v8::Maybe<int32_t> result =
        CompileRun("f()")->Int32Value(isolate1->GetCurrentContext());
    CHECK_EQ(42, result.FromJust());
  }
  isolate1->Dispose();
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobStringNotInternalized) {
  DisableAlwaysOpt();
  const char* source1 =
      R"javascript(
      // String would be internalized if it came from a literal so create "AB"
      // via a function call.
      var global = String.fromCharCode(65, 66);
      function f() { return global; }
      )javascript";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate1 = TestSerializer::NewIsolate(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope c_scope(context);
    v8::Local<v8::Value> result = CompileRun("f()").As<v8::Value>();
    CHECK(result->IsString());
    i::String str = *v8::Utils::OpenHandle(*result.As<v8::String>());
    CHECK_EQ(std::string(str.ToCString().get()), "AB");
    CHECK(!str.IsInternalizedString());
    CHECK(!i::ReadOnlyHeap::Contains(str));
  }
  isolate1->Dispose();
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

namespace {

void TestCustomSnapshotDataBlobWithIrregexpCode(
    v8::SnapshotCreator::FunctionCodeHandling function_code_handling) {
  DisableAlwaysOpt();
  const char* source =
      "var re1 = /\\/\\*[^*]*\\*+([^/*][^*]*\\*+)*\\//;\n"
      "function f() { return '/* a comment */'.search(re1); }\n"
      "function g() { return 'not a comment'.search(re1); }\n"
      "function h() { return '// this is a comment'.search(re1); }\n"
      "var re2 = /a/;\n"
      "function i() { return '/* a comment */'.search(re2); }\n"
      "f(); f(); g(); g(); h(); h(); i(); i();\n";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 =
      CreateSnapshotDataBlobInternal(function_code_handling, source);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate1 = TestSerializer::NewIsolate(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope c_scope(context);
    {
      // Check that compiled irregexp code has not been flushed prior to
      // serialization.
      i::Handle<i::JSRegExp> re =
          Utils::OpenHandle(*CompileRun("re1").As<v8::RegExp>());
      CHECK_EQ(re->HasCompiledCode(),
               function_code_handling ==
                   v8::SnapshotCreator::FunctionCodeHandling::kKeep);
    }
    {
      v8::Maybe<int32_t> result =
          CompileRun("f()")->Int32Value(isolate1->GetCurrentContext());
      CHECK_EQ(0, result.FromJust());
    }
    {
      v8::Maybe<int32_t> result =
          CompileRun("g()")->Int32Value(isolate1->GetCurrentContext());
      CHECK_EQ(-1, result.FromJust());
    }
    {
      v8::Maybe<int32_t> result =
          CompileRun("h()")->Int32Value(isolate1->GetCurrentContext());
      CHECK_EQ(-1, result.FromJust());
    }
    {
      // Check that ATOM regexp remains valid.
      i::Handle<i::JSRegExp> re =
          Utils::OpenHandle(*CompileRun("re2").As<v8::RegExp>());
      CHECK_EQ(re->type_tag(), JSRegExp::ATOM);
      CHECK(!re->HasCompiledCode());
    }
  }
  isolate1->Dispose();
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

}  // namespace

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithIrregexpCodeKeepCode) {
  TestCustomSnapshotDataBlobWithIrregexpCode(
      v8::SnapshotCreator::FunctionCodeHandling::kKeep);
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithIrregexpCodeClearCode) {
  TestCustomSnapshotDataBlobWithIrregexpCode(
      v8::SnapshotCreator::FunctionCodeHandling::kClear);
}

UNINITIALIZED_TEST(SnapshotChecksum) {
  DisableAlwaysOpt();
  const char* source1 = "function f() { return 42; }";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);
  CHECK(i::Snapshot::VerifyChecksum(&data1));
  const_cast<char*>(data1.data)[142] = data1.data[142] ^ 4;  // Flip a bit.
  CHECK(!i::Snapshot::VerifyChecksum(&data1));
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

struct InternalFieldData {
  uint32_t data;
};

v8::StartupData SerializeInternalFields(v8::Local<v8::Object> holder, int index,
                                        void* data) {
  if (data == reinterpret_cast<void*>(2000)) {
    // Used for SnapshotCreatorTemplates test. We check that none of the fields
    // have been cleared yet.
    CHECK_NOT_NULL(holder->GetAlignedPointerFromInternalField(1));
  } else {
    CHECK_EQ(reinterpret_cast<void*>(2016), data);
  }
  if (index != 1) return {nullptr, 0};
  InternalFieldData* embedder_field = static_cast<InternalFieldData*>(
      holder->GetAlignedPointerFromInternalField(index));
  if (embedder_field == nullptr) return {nullptr, 0};
  int size = sizeof(*embedder_field);
  char* payload = new char[size];
  // We simply use memcpy to serialize the content.
  memcpy(payload, embedder_field, size);
  return {payload, size};
}

std::vector<InternalFieldData*> deserialized_data;

void DeserializeInternalFields(v8::Local<v8::Object> holder, int index,
                               v8::StartupData payload, void* data) {
  if (payload.raw_size == 0) {
    holder->SetAlignedPointerInInternalField(index, nullptr);
    return;
  }
  CHECK_EQ(reinterpret_cast<void*>(2017), data);
  InternalFieldData* embedder_field = new InternalFieldData{0};
  memcpy(embedder_field, payload.data, payload.raw_size);
  holder->SetAlignedPointerInInternalField(index, embedder_field);
  deserialized_data.push_back(embedder_field);
}

using Int32Expectations = std::vector<std::tuple<const char*, int32_t>>;

void TestInt32Expectations(const Int32Expectations& expectations) {
  for (const auto& e : expectations) {
    ExpectInt32(std::get<0>(e), std::get<1>(e));
  }
}

void TypedArrayTestHelper(
    const char* code, const Int32Expectations& expectations,
    const char* code_to_run_after_restore = nullptr,
    const Int32Expectations& after_restore_expectations = Int32Expectations(),
    v8::ArrayBuffer::Allocator* allocator = nullptr) {
  DisableAlwaysOpt();
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      CompileRun(code);
      TestInt32Expectations(expectations);
      creator.SetDefaultContext(
          context, v8::SerializeInternalFieldsCallback(
                       SerializeInternalFields, reinterpret_cast<void*>(2016)));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.snapshot_blob = &blob;
  create_params.array_buffer_allocator =
      allocator != nullptr ? allocator : CcTest::array_buffer_allocator();
  v8::Isolate* isolate = TestSerializer::NewIsolate(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(
        isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
        v8::MaybeLocal<v8::Value>(),
        v8::DeserializeInternalFieldsCallback(DeserializeInternalFields,
                                              reinterpret_cast<void*>(2017)));
    CHECK(deserialized_data.empty());  // We do not expect any embedder data.
    v8::Context::Scope c_scope(context);
    TestInt32Expectations(expectations);
    if (code_to_run_after_restore) {
      CompileRun(code_to_run_after_restore);
    }
    TestInt32Expectations(after_restore_expectations);
  }
  isolate->Dispose();
  delete[] blob.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithOffHeapTypedArray) {
  const char* code =
      "var x = new Uint8Array(128);"
      "x[0] = 12;"
      "var arr = new Array(17);"
      "arr[1] = 24;"
      "var y = new Uint32Array(arr);"
      "var buffer = new ArrayBuffer(128);"
      "var z = new Int16Array(buffer);"
      "z[0] = 48;";
  Int32Expectations expectations = {std::make_tuple("x[0]", 12),
                                    std::make_tuple("y[1]", 24),
                                    std::make_tuple("z[0]", 48)};

  TypedArrayTestHelper(code, expectations);
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobSharedArrayBuffer) {
  const char* code =
      "var x = new Int32Array([12, 24, 48, 96]);"
      "var y = new Uint8Array(x.buffer)";
  Int32Expectations expectations = {
    std::make_tuple("x[0]", 12),
    std::make_tuple("x[1]", 24),
#if !V8_TARGET_BIG_ENDIAN
    std::make_tuple("y[0]", 12),
    std::make_tuple("y[1]", 0),
    std::make_tuple("y[2]", 0),
    std::make_tuple("y[3]", 0),
    std::make_tuple("y[4]", 24)
#else
    std::make_tuple("y[3]", 12),
    std::make_tuple("y[2]", 0),
    std::make_tuple("y[1]", 0),
    std::make_tuple("y[0]", 0),
    std::make_tuple("y[7]", 24)
#endif
  };

  TypedArrayTestHelper(code, expectations);
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobArrayBufferWithOffset) {
  const char* code =
      "var x = new Int32Array([12, 24, 48, 96]);"
      "var y = new Int32Array(x.buffer, 4, 2)";
  Int32Expectations expectations = {
      std::make_tuple("x[1]", 24),
      std::make_tuple("x[2]", 48),
      std::make_tuple("y[0]", 24),
      std::make_tuple("y[1]", 48),
  };

  // Verify that the typed arrays use the same buffer (not independent copies).
  const char* code_to_run_after_restore = "x[2] = 57; y[0] = 42;";
  Int32Expectations after_restore_expectations = {
      std::make_tuple("x[1]", 42),
      std::make_tuple("y[1]", 57),
  };

  TypedArrayTestHelper(code, expectations, code_to_run_after_restore,
                       after_restore_expectations);
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobDataView) {
  const char* code =
      "var x = new Int8Array([1, 2, 3, 4]);"
      "var v = new DataView(x.buffer)";
  Int32Expectations expectations = {std::make_tuple("v.getInt8(0)", 1),
                                    std::make_tuple("v.getInt8(1)", 2),
                                    std::make_tuple("v.getInt16(0)", 258),
                                    std::make_tuple("v.getInt16(1)", 515)};

  TypedArrayTestHelper(code, expectations);
}

namespace {
class AlternatingArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  AlternatingArrayBufferAllocator()
      : allocation_fails_(false),
        allocator_(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {}
  ~AlternatingArrayBufferAllocator() { delete allocator_; }
  void* Allocate(size_t length) override {
    allocation_fails_ = !allocation_fails_;
    if (allocation_fails_) return nullptr;
    return allocator_->Allocate(length);
  }

  void* AllocateUninitialized(size_t length) override {
    return this->Allocate(length);
  }

  void Free(void* data, size_t size) override { allocator_->Free(data, size); }

  void* Reallocate(void* data, size_t old_length, size_t new_length) override {
    return allocator_->Reallocate(data, old_length, new_length);
  }

 private:
  bool allocation_fails_;
  v8::ArrayBuffer::Allocator* allocator_;
};
}  // anonymous namespace

UNINITIALIZED_TEST(CustomSnapshotManyArrayBuffers) {
  const char* code =
      "var buffers = [];"
      "for (let i = 0; i < 70; i++) buffers.push(new Uint8Array(1000));";
  Int32Expectations expectations = {std::make_tuple("buffers.length", 70)};
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      new AlternatingArrayBufferAllocator());
  TypedArrayTestHelper(code, expectations, nullptr, Int32Expectations(),
                       allocator.get());
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobDetachedArrayBuffer) {
  const char* code =
      "var x = new Int16Array([12, 24, 48]);"
      "%ArrayBufferDetach(x.buffer);";
  Int32Expectations expectations = {std::make_tuple("x.buffer.byteLength", 0),
                                    std::make_tuple("x.length", 0)};

  DisableAlwaysOpt();
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      CompileRun(code);
      TestInt32Expectations(expectations);
      creator.SetDefaultContext(
          context, v8::SerializeInternalFieldsCallback(
                       SerializeInternalFields, reinterpret_cast<void*>(2016)));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.snapshot_blob = &blob;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = TestSerializer::NewIsolate(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(
        isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
        v8::MaybeLocal<v8::Value>(),
        v8::DeserializeInternalFieldsCallback(DeserializeInternalFields,
                                              reinterpret_cast<void*>(2017)));
    v8::Context::Scope c_scope(context);
    TestInt32Expectations(expectations);

    v8::Local<v8::Value> x = CompileRun("x");
    CHECK(x->IsTypedArray());
    i::Handle<i::JSTypedArray> array =
        i::Handle<i::JSTypedArray>::cast(v8::Utils::OpenHandle(*x));
    CHECK(array->WasDetached());
  }
  isolate->Dispose();
  delete[] blob.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

i::Handle<i::JSArrayBuffer> GetBufferFromTypedArray(
    v8::Local<v8::Value> typed_array) {
  CHECK(typed_array->IsTypedArray());

  i::Handle<i::JSArrayBufferView> view = i::Handle<i::JSArrayBufferView>::cast(
      v8::Utils::OpenHandle(*typed_array));

  return i::handle(i::JSArrayBuffer::cast(view->buffer()), view->GetIsolate());
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobOnOrOffHeapTypedArray) {
  const char* code =
      "var x = new Uint8Array(8);"
      "x[0] = 12;"
      "x[7] = 24;"
      "var y = new Int16Array([12, 24, 48]);"
      "var z = new Int32Array(64);"
      "z[0] = 96;";
  Int32Expectations expectations = {
      std::make_tuple("x[0]", 12), std::make_tuple("x[7]", 24),
      std::make_tuple("y[2]", 48), std::make_tuple("z[0]", 96)};

  DisableAlwaysOpt();
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      CompileRun(code);
      TestInt32Expectations(expectations);
      i::Handle<i::JSArrayBuffer> buffer =
          GetBufferFromTypedArray(CompileRun("x"));
      // The resulting buffer should be on-heap.
      CHECK(buffer->IsEmpty());
      creator.SetDefaultContext(
          context, v8::SerializeInternalFieldsCallback(
                       SerializeInternalFields, reinterpret_cast<void*>(2016)));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.snapshot_blob = &blob;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = TestSerializer::NewIsolate(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(
        isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
        v8::MaybeLocal<v8::Value>(),
        v8::DeserializeInternalFieldsCallback(DeserializeInternalFields,
                                              reinterpret_cast<void*>(2017)));
    v8::Context::Scope c_scope(context);
    TestInt32Expectations(expectations);

    i::Handle<i::JSArrayBuffer> buffer =
        GetBufferFromTypedArray(CompileRun("x"));
    // The resulting buffer should be on-heap.
    CHECK(buffer->IsEmpty());

    buffer = GetBufferFromTypedArray(CompileRun("y"));
    CHECK(buffer->IsEmpty());

    buffer = GetBufferFromTypedArray(CompileRun("z"));
    // The resulting buffer should be off-heap.
    CHECK(!buffer->IsEmpty());
  }
  isolate->Dispose();
  delete[] blob.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobTypedArrayNoEmbedderFieldCallback) {
  const char* code = "var x = new Uint8Array(8);";
  DisableAlwaysOpt();
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      CompileRun(code);
      creator.SetDefaultContext(context, v8::SerializeInternalFieldsCallback());
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.snapshot_blob = &blob;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = TestSerializer::NewIsolate(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(
        isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
        v8::MaybeLocal<v8::Value>(), v8::DeserializeInternalFieldsCallback());
    v8::Context::Scope c_scope(context);
  }
  isolate->Dispose();
  delete[] blob.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlob2) {
  DisableAlwaysOpt();
  const char* source2 =
      "function f() { return g() * 2; }"
      "function g() { return 43; }"
      "/./.test('a')";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data2 = CreateSnapshotDataBlob(source2);

  v8::Isolate::CreateParams params2;
  params2.snapshot_blob = &data2;
  params2.array_buffer_allocator = CcTest::array_buffer_allocator();
  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate2 = TestSerializer::NewIsolate(params2);
  {
    v8::Isolate::Scope i_scope(isolate2);
    v8::HandleScope h_scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope c_scope(context);
    v8::Maybe<int32_t> result =
        CompileRun("f()")->Int32Value(isolate2->GetCurrentContext());
    CHECK_EQ(86, result.FromJust());
    result = CompileRun("g()")->Int32Value(isolate2->GetCurrentContext());
    CHECK_EQ(43, result.FromJust());
  }
  isolate2->Dispose();
  delete[] data2.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

static void SerializationFunctionTemplate(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::ValidateCallbackInfo(info));
  info.GetReturnValue().Set(info[0]);
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobOutdatedContextWithOverflow) {
  DisableAlwaysOpt();
  const char* source1 =
      "var o = {};"
      "(function() {"
      "  function f1(x) { return f2(x) instanceof Array; }"
      "  function f2(x) { return foo.bar(x); }"
      "  o.a = f2.bind(null);"
      "  o.b = 1;"
      "  o.c = 2;"
      "  o.d = 3;"
      "  o.e = 4;"
      "})();\n";

  const char* source2 = "o.a(42)";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &data;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);

    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::ObjectTemplate> property = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::FunctionTemplate> function =
        v8::FunctionTemplate::New(isolate, SerializationFunctionTemplate);
    property->Set(isolate, "bar", function);
    global->Set(isolate, "foo", property);

    v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global);
    v8::Context::Scope c_scope(context);
    v8::Local<v8::Value> result = CompileRun(source2);
    v8::Maybe<bool> compare =
        v8_str("42")->Equals(isolate->GetCurrentContext(), result);
    CHECK(compare.FromJust());
  }
  isolate->Dispose();
  delete[] data.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithLocker) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate0 = v8::Isolate::New(create_params);
  {
    v8::Locker locker(isolate0);
    v8::Isolate::Scope i_scope(isolate0);
    v8::HandleScope h_scope(isolate0);
    v8::Local<v8::Context> context = v8::Context::New(isolate0);
    v8::Context::Scope c_scope(context);
    v8::Maybe<int32_t> result =
        CompileRun("Math.cos(0)")->Int32Value(isolate0->GetCurrentContext());
    CHECK_EQ(1, result.FromJust());
  }
  isolate0->Dispose();

  const char* source1 = "function f() { return 42; }";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();
  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate1 = TestSerializer::NewIsolate(params1);
  {
    v8::Locker locker(isolate1);
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope c_scope(context);
    v8::Maybe<int32_t> result = CompileRun("f()")->Int32Value(context);
    CHECK_EQ(42, result.FromJust());
  }
  isolate1->Dispose();
  delete[] data1.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobStackOverflow) {
  DisableAlwaysOpt();
  const char* source =
      "var a = [0];"
      "var b = a;"
      "for (var i = 0; i < 10000; i++) {"
      "  var c = [i];"
      "  b.push(c);"
      "  b.push(c);"
      "  b = c;"
      "}";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data = CreateSnapshotDataBlob(source);

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &data;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope c_scope(context);
    const char* test =
        "var sum = 0;"
        "while (a) {"
        "  sum += a[0];"
        "  a = a[1];"
        "}"
        "sum";
    v8::Maybe<int32_t> result =
        CompileRun(test)->Int32Value(isolate->GetCurrentContext());
    CHECK_EQ(9999 * 5000, result.FromJust());
  }
  isolate->Dispose();
  delete[] data.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

bool IsCompiled(const char* name) {
  return i::Handle<i::JSFunction>::cast(
             v8::Utils::OpenHandle(*CompileRun(name)))
      ->shared()
      .is_compiled();
}

UNINITIALIZED_TEST(SnapshotDataBlobWithWarmup) {
  DisableAlwaysOpt();
  const char* warmup = "Math.abs(1); Math.random = 1;";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData cold = CreateSnapshotDataBlob(nullptr);
  v8::StartupData warm = WarmUpSnapshotDataBlobInternal(cold, warmup);
  delete[] cold.data;

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &warm;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope c_scope(context);
    // Running the warmup script has effect on whether functions are
    // pre-compiled, but does not pollute the context.
    CHECK(IsCompiled("Math.abs"));
    CHECK(IsCompiled("String.raw"));
    CHECK(CompileRun("Math.random")->IsFunction());
  }
  isolate->Dispose();
  delete[] warm.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithWarmup) {
  DisableAlwaysOpt();
  const char* source =
      "function f() { return Math.abs(1); }\n"
      "function g() { return String.raw(1); }\n"
      "Object.valueOf(1);"
      "var a = 5";
  const char* warmup = "a = f()";

  DisableEmbeddedBlobRefcounting();
  v8::StartupData cold = CreateSnapshotDataBlob(source);
  v8::StartupData warm = WarmUpSnapshotDataBlobInternal(cold, warmup);
  delete[] cold.data;

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &warm;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope c_scope(context);
    // Running the warmup script has effect on whether functions are
    // pre-compiled, but does not pollute the context.
    CHECK(IsCompiled("f"));
    CHECK(IsCompiled("Math.abs"));
    CHECK(!IsCompiled("g"));
    CHECK(IsCompiled("String.raw"));
    CHECK(IsCompiled("Array.prototype.lastIndexOf"));
    CHECK_EQ(5, CompileRun("a")->Int32Value(context).FromJust());
  }
  isolate->Dispose();
  delete[] warm.data;
  FreeCurrentEmbeddedBlob();
}

namespace {
v8::StartupData CreateCustomSnapshotWithKeep() {
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::Local<v8::String> source_str = v8_str(
          "function f() { return Math.abs(1); }\n"
          "function g() { return String.raw(1); }");
      v8::ScriptOrigin origin(isolate, v8_str("test"));
      v8::ScriptCompiler::Source source(source_str, origin);
      CompileRun(isolate->GetCurrentContext(), &source,
                 v8::ScriptCompiler::kEagerCompile);
      creator.SetDefaultContext(context);
    }
  }
  return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);
}
}  // namespace

UNINITIALIZED_TEST(CustomSnapshotDataBlobWithKeep) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob = CreateCustomSnapshotWithKeep();

  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CHECK(IsCompiled("f"));
      CHECK(IsCompiled("g"));
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(CustomSnapshotDataBlobImmortalImmovableRoots) {
  DisableAlwaysOpt();
  // Flood the startup snapshot with shared function infos. If they are
  // serialized before the immortal immovable root, the root will no longer end
  // up on the first page.
  base::Vector<const char> source =
      ConstructSource(base::StaticCharVector("var a = [];"),
                      base::StaticCharVector("a.push(function() {return 7});"),
                      base::StaticCharVector("\0"), 10000);

  DisableEmbeddedBlobRefcounting();
  v8::StartupData data = CreateSnapshotDataBlob(source.begin());

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &data;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();

  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope c_scope(context);
    CHECK_EQ(7, CompileRun("a[0]()")->Int32Value(context).FromJust());
  }
  isolate->Dispose();
  source.Dispose();
  delete[] data.data;  // We can dispose of the snapshot blob now.
  FreeCurrentEmbeddedBlob();
}

TEST(TestThatAlwaysSucceeds) {}

TEST(TestThatAlwaysFails) {
  bool ArtificialFailure = false;
  CHECK(ArtificialFailure);
}

int CountBuiltins() {
  // Check that we have not deserialized any additional builtin.
  HeapObjectIterator iterator(CcTest::heap());
  DisallowGarbageCollection no_gc;
  int counter = 0;
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    if (obj.IsCode() && Code::cast(obj).kind() == CodeKind::BUILTIN) counter++;
  }
  return counter;
}

static Handle<SharedFunctionInfo> CompileScript(
    Isolate* isolate, Handle<String> source,
    const ScriptDetails& script_details, AlignedCachedData* cached_data,
    v8::ScriptCompiler::CompileOptions options) {
  return Compiler::GetSharedFunctionInfoForScriptWithCachedData(
             isolate, source, script_details, cached_data, options,
             ScriptCompiler::kNoCacheNoReason, NOT_NATIVES_CODE)
      .ToHandleChecked();
}

static Handle<SharedFunctionInfo> CompileScriptAndProduceCache(
    Isolate* isolate, Handle<String> source,
    const ScriptDetails& script_details, AlignedCachedData** out_cached_data,
    v8::ScriptCompiler::CompileOptions options) {
  Handle<SharedFunctionInfo> sfi =
      Compiler::GetSharedFunctionInfoForScript(
          isolate, source, script_details, options,
          ScriptCompiler::kNoCacheNoReason, NOT_NATIVES_CODE)
          .ToHandleChecked();
  std::unique_ptr<ScriptCompiler::CachedData> cached_data(
      ScriptCompiler::CreateCodeCache(ToApiHandle<UnboundScript>(sfi)));
  uint8_t* buffer = NewArray<uint8_t>(cached_data->length);
  MemCopy(buffer, cached_data->data, cached_data->length);
  *out_cached_data = new i::AlignedCachedData(buffer, cached_data->length);
  (*out_cached_data)->AcquireDataOwnership();
  return sfi;
}

TEST(CodeSerializerWithProfiler) {
  v8_flags.enable_lazy_source_positions = true;
  v8_flags.stress_lazy_source_positions = false;

  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  const char* source = "1 + 1";

  Handle<String> orig_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  Handle<String> copy_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  CHECK(!orig_source.is_identical_to(copy_source));
  CHECK(orig_source->Equals(*copy_source));

  AlignedCachedData* cache = nullptr;

  ScriptDetails default_script_details;
  Handle<SharedFunctionInfo> orig = CompileScriptAndProduceCache(
      isolate, orig_source, default_script_details, &cache,
      v8::ScriptCompiler::kNoCompileOptions);

  CHECK(!orig->GetBytecodeArray(isolate).HasSourcePositionTable());

  isolate->SetIsProfiling(true);

  // This does not assert that no compilation can happen as source position
  // collection could trigger it.
  Handle<SharedFunctionInfo> copy =
      CompileScript(isolate, copy_source, default_script_details, cache,
                    v8::ScriptCompiler::kConsumeCodeCache);

  // Since the profiler is now enabled, source positions should be collected
  // after deserialization.
  CHECK(copy->GetBytecodeArray(isolate).HasSourcePositionTable());

  delete cache;
}

void TestCodeSerializerOnePlusOneImpl(bool verify_builtins_count = true) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  const char* source = "1 + 1";

  Handle<String> orig_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  Handle<String> copy_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  CHECK(!orig_source.is_identical_to(copy_source));
  CHECK(orig_source->Equals(*copy_source));

  AlignedCachedData* cache = nullptr;

  ScriptDetails default_script_details;
  Handle<SharedFunctionInfo> orig = CompileScriptAndProduceCache(
      isolate, orig_source, default_script_details, &cache,
      v8::ScriptCompiler::kNoCompileOptions);

  int builtins_count = CountBuiltins();

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, copy_source, default_script_details, cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }

  CHECK_NE(*orig, *copy);
  CHECK(Script::cast(copy->script()).source() == *copy_source);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();
  Handle<JSObject> global(isolate->context().global_object(), isolate);
  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();
  CHECK_EQ(2, Handle<Smi>::cast(copy_result)->value());

  if (verify_builtins_count) CHECK_EQ(builtins_count, CountBuiltins());

  delete cache;
}

TEST(CodeSerializerOnePlusOne) { TestCodeSerializerOnePlusOneImpl(); }

// See bug v8:9122
TEST(CodeSerializerOnePlusOneWithInterpretedFramesNativeStack) {
  v8_flags.interpreted_frames_native_stack = true;
  // We pass false because this test will create IET copies (which are
  // builtins).
  TestCodeSerializerOnePlusOneImpl(false);
}

TEST(CodeSerializerOnePlusOneWithDebugger) {
  v8::HandleScope scope(CcTest::isolate());
  static v8::debug::DebugDelegate dummy_delegate;
  v8::debug::SetDebugDelegate(CcTest::isolate(), &dummy_delegate);
  TestCodeSerializerOnePlusOneImpl();
}

TEST(CodeSerializerPromotedToCompilationCache) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();

  v8::HandleScope scope(CcTest::isolate());

  const char* source = "1 + 1";

  Handle<String> src = isolate->factory()->NewStringFromAsciiChecked(source);
  AlignedCachedData* cache = nullptr;

  Handle<FixedArray> default_host_defined_options =
      isolate->factory()->NewFixedArray(2);
  default_host_defined_options->set(0, Smi::FromInt(0));
  const char* default_host_defined_option_1_string = "custom string";
  Handle<String> default_host_defined_option_1 =
      isolate->factory()->NewStringFromAsciiChecked(
          default_host_defined_option_1_string);
  default_host_defined_options->set(1, *default_host_defined_option_1);

  ScriptDetails default_script_details(src);
  default_script_details.host_defined_options = default_host_defined_options;
  CompileScriptAndProduceCache(isolate, src, default_script_details, &cache,
                               v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, src, default_script_details, cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }

  {
    ScriptDetails script_details(src);
    script_details.host_defined_options =
        default_script_details.host_defined_options;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK_EQ(*lookup_result.toplevel_sfi().ToHandleChecked(), *copy);
  }

  {
    // Lookup with strictly equal host_defined_options should succeed:
    ScriptDetails script_details(src);
    Handle<FixedArray> host_defined_options =
        isolate->factory()->NewFixedArray(2);
    host_defined_options->set(0, default_host_defined_options->get(0));
    Handle<String> host_defined_option_1 =
        isolate->factory()->NewStringFromAsciiChecked(
            default_host_defined_option_1_string);
    host_defined_options->set(1, *host_defined_option_1);
    script_details.host_defined_options = host_defined_options;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK_EQ(*lookup_result.toplevel_sfi().ToHandleChecked(), *copy);
  }

  {
    // Lookup with different string with same contents should succeed:
    ScriptDetails script_details(
        isolate->factory()->NewStringFromAsciiChecked(source));
    script_details.host_defined_options =
        default_script_details.host_defined_options;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK_EQ(*lookup_result.toplevel_sfi().ToHandleChecked(), *copy);
  }

  {
    // Lookup with different name string should fail:
    ScriptDetails script_details(
        isolate->factory()->NewStringFromAsciiChecked("other"));
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  {
    // Lookup with different position should fail:
    ScriptDetails script_details(src);
    script_details.line_offset = 0xFF;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  {
    // Lookup with different position should fail:
    ScriptDetails script_details(src);
    script_details.column_offset = 0xFF;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  {
    // Lookup with different language mode should fail:
    ScriptDetails script_details(src);
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kStrict);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  {
    // Lookup with different script_options should fail
    ScriptOriginOptions origin_options(false, true);
    CHECK_NE(ScriptOriginOptions().Flags(), origin_options.Flags());
    ScriptDetails script_details(src, origin_options);
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  {
    // Lookup with different host_defined_options should fail:
    ScriptDetails script_details(src);
    script_details.host_defined_options = isolate->factory()->NewFixedArray(5);
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK(lookup_result.script().is_null() &&
          lookup_result.toplevel_sfi().is_null());
  }

  // Compile the script again with different options.
  ScriptDetails alternative_script_details(src);
  Handle<SharedFunctionInfo> alternative_toplevel_sfi =
      Compiler::GetSharedFunctionInfoForScript(
          isolate, src, alternative_script_details,
          ScriptCompiler::kNoCompileOptions, ScriptCompiler::kNoCacheNoReason,
          NOT_NATIVES_CODE)
          .ToHandleChecked();
  CHECK_NE(*copy, *alternative_toplevel_sfi);

  {
    // The original script can still be found.
    ScriptDetails script_details(src);
    script_details.host_defined_options =
        default_script_details.host_defined_options;
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK_EQ(*lookup_result.toplevel_sfi().ToHandleChecked(), *copy);
  }

  {
    // The new script can also be found.
    ScriptDetails script_details(src);
    auto lookup_result = isolate->compilation_cache()->LookupScript(
        src, script_details, LanguageMode::kSloppy);
    CHECK_EQ(*lookup_result.toplevel_sfi().ToHandleChecked(),
             *alternative_toplevel_sfi);
  }

  delete cache;
}

TEST(CodeSerializerInternalizedString) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  const char* source = "'string1'";

  Handle<String> orig_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  Handle<String> copy_source = isolate->factory()
                                   ->NewStringFromUtf8(base::CStrVector(source))
                                   .ToHandleChecked();
  CHECK(!orig_source.is_identical_to(copy_source));
  CHECK(orig_source->Equals(*copy_source));

  Handle<JSObject> global(isolate->context().global_object(), isolate);

  i::AlignedCachedData* cached_data = nullptr;
  Handle<SharedFunctionInfo> orig = CompileScriptAndProduceCache(
      isolate, orig_source, ScriptDetails(), &cached_data,
      v8::ScriptCompiler::kNoCompileOptions);
  Handle<JSFunction> orig_fun =
      Factory::JSFunctionBuilder{isolate, orig, isolate->native_context()}
          .Build();
  Handle<Object> orig_result =
      Execution::CallScript(isolate, orig_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();
  CHECK(orig_result->IsInternalizedString());

  int builtins_count = CountBuiltins();

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, copy_source, ScriptDetails(), cached_data,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);
  CHECK(Script::cast(copy->script()).source() == *copy_source);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();
  CHECK_NE(*orig_fun, *copy_fun);
  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();
  CHECK(orig_result.is_identical_to(copy_result));
  Handle<String> expected =
      isolate->factory()->NewStringFromAsciiChecked("string1");

  CHECK(Handle<String>::cast(copy_result)->Equals(*expected));
  CHECK_EQ(builtins_count, CountBuiltins());

  delete cached_data;
}

TEST(CodeSerializerLargeCodeObject) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  // The serializer only tests the shared code, which is always the unoptimized
  // code. Don't even bother generating optimized code to avoid timeouts.
  v8_flags.always_turbofan = false;

  base::Vector<const char> source = ConstructSource(
      base::StaticCharVector("var j=1; if (j == 0) {"),
      base::StaticCharVector(
          "for (let i of Object.prototype) for (let k = 0; k < 0; ++k);"),
      base::StaticCharVector("} j=7; j"), 2000);
  Handle<String> source_str =
      isolate->factory()->NewStringFromUtf8(source).ToHandleChecked();

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig =
      CompileScriptAndProduceCache(isolate, source_str, ScriptDetails(), &cache,
                                   v8::ScriptCompiler::kNoCompileOptions);

  CHECK(isolate->heap()->InSpace(orig->abstract_code(isolate), LO_SPACE));

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_str, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  int result_int;
  CHECK(copy_result->ToInt32(&result_int));
  CHECK_EQ(7, result_int);

  delete cache;
  source.Dispose();
}

TEST(CodeSerializerLargeCodeObjectWithIncrementalMarking) {
  if (!v8_flags.incremental_marking) return;
  if (!v8_flags.compact) return;
  ManualGCScope manual_gc_scope;
  v8_flags.always_turbofan = false;
  const char* filter_flag = "--turbo-filter=NOTHING";
  FlagList::SetFlagsFromString(filter_flag, strlen(filter_flag));
  v8_flags.manual_evacuation_candidates_selection = true;

  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  Heap* heap = isolate->heap();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  base::Vector<const char> source = ConstructSource(
      base::StaticCharVector("var j=1; if (j == 0) {"),
      base::StaticCharVector("for (var i = 0; i < Object.prototype; i++);"),
      base::StaticCharVector("} j=7; var s = 'happy_hippo'; j"), 20000);
  Handle<String> source_str =
      isolate->factory()->NewStringFromUtf8(source).ToHandleChecked();

  // Create a string on an evacuation candidate in old space.
  Handle<String> moving_object;
  Page* ec_page;
  {
    AlwaysAllocateScopeForTesting always_allocate(heap);
    heap::SimulateFullSpace(heap->old_space());
    moving_object = isolate->factory()->InternalizeString(
        isolate->factory()->NewStringFromAsciiChecked("happy_hippo"));
    ec_page = Page::FromHeapObject(*moving_object);
  }

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig =
      CompileScriptAndProduceCache(isolate, source_str, ScriptDetails(), &cache,
                                   v8::ScriptCompiler::kNoCompileOptions);

  CHECK(heap->InSpace(orig->abstract_code(isolate), LO_SPACE));

  // Pretend that incremental marking is on when deserialization begins.
  heap::ForceEvacuationCandidate(ec_page);
  heap::SimulateIncrementalMarking(heap, false);
  IncrementalMarking* marking = heap->incremental_marking();
  CHECK(marking->black_allocation());
  CHECK(marking->IsCompacting());
  CHECK(MarkCompactCollector::IsOnEvacuationCandidate(*moving_object));

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_str, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  // We should have missed a write barrier. Complete incremental marking
  // to flush out the bug.
  heap::SimulateIncrementalMarking(heap, true);
  CcTest::CollectAllGarbage();

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  int result_int;
  CHECK(copy_result->ToInt32(&result_int));
  CHECK_EQ(7, result_int);

  delete cache;
  source.Dispose();
}

TEST(CodeSerializerLargeStrings) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  Factory* f = isolate->factory();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  base::Vector<const char> source_s = ConstructSource(
      base::StaticCharVector("var s = \""), base::StaticCharVector("abcdef"),
      base::StaticCharVector("\";"), 1000000);
  base::Vector<const char> source_t = ConstructSource(
      base::StaticCharVector("var t = \""), base::StaticCharVector("uvwxyz"),
      base::StaticCharVector("\"; s + t"), 999999);
  Handle<String> source_str =
      f->NewConsString(f->NewStringFromUtf8(source_s).ToHandleChecked(),
                       f->NewStringFromUtf8(source_t).ToHandleChecked())
          .ToHandleChecked();

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig =
      CompileScriptAndProduceCache(isolate, source_str, ScriptDetails(), &cache,
                                   v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_str, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  CHECK_EQ(6 * 1999999, Handle<String>::cast(copy_result)->length());
  Handle<Object> property = JSReceiver::GetDataProperty(
      isolate, isolate->global_object(), f->NewStringFromAsciiChecked("s"));
  CHECK(isolate->heap()->InSpace(HeapObject::cast(*property), LO_SPACE));
  property = JSReceiver::GetDataProperty(isolate, isolate->global_object(),
                                         f->NewStringFromAsciiChecked("t"));
  CHECK(isolate->heap()->InSpace(HeapObject::cast(*property), LO_SPACE));
  // Make sure we do not serialize too much, e.g. include the source string.
  CHECK_LT(cache->length(), 13000000);

  delete cache;
  source_s.Dispose();
  source_t.Dispose();
}

TEST(CodeSerializerThreeBigStrings) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  Factory* f = isolate->factory();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  const int32_t length_of_a = kMaxRegularHeapObjectSize * 2;
  const int32_t length_of_b = kMaxRegularHeapObjectSize / 2;
  const int32_t length_of_c = kMaxRegularHeapObjectSize / 2;

  base::Vector<const char> source_a = ConstructSource(
      base::StaticCharVector("var a = \""), base::StaticCharVector("a"),
      base::StaticCharVector("\";"), length_of_a);
  Handle<String> source_a_str =
      f->NewStringFromUtf8(source_a).ToHandleChecked();

  base::Vector<const char> source_b = ConstructSource(
      base::StaticCharVector("var b = \""), base::StaticCharVector("b"),
      base::StaticCharVector("\";"), length_of_b);
  Handle<String> source_b_str =
      f->NewStringFromUtf8(source_b).ToHandleChecked();

  base::Vector<const char> source_c = ConstructSource(
      base::StaticCharVector("var c = \""), base::StaticCharVector("c"),
      base::StaticCharVector("\";"), length_of_c);
  Handle<String> source_c_str =
      f->NewStringFromUtf8(source_c).ToHandleChecked();

  Handle<String> source_str =
      f->NewConsString(
           f->NewConsString(source_a_str, source_b_str).ToHandleChecked(),
           source_c_str)
          .ToHandleChecked();

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig =
      CompileScriptAndProduceCache(isolate, source_str, ScriptDetails(), &cache,
                                   v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_str, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  USE(Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array()));

  v8::Maybe<int32_t> result =
      CompileRun("(a + b).length")
          ->Int32Value(CcTest::isolate()->GetCurrentContext());
  CHECK_EQ(length_of_a + length_of_b, result.FromJust());
  result = CompileRun("(b + c).length")
               ->Int32Value(CcTest::isolate()->GetCurrentContext());
  CHECK_EQ(length_of_b + length_of_c, result.FromJust());
  Heap* heap = isolate->heap();
  v8::Local<v8::String> result_str =
      CompileRun("a")
          ->ToString(CcTest::isolate()->GetCurrentContext())
          .ToLocalChecked();
  CHECK(heap->InSpace(*v8::Utils::OpenHandle(*result_str), LO_SPACE));
  result_str = CompileRun("b")
                   ->ToString(CcTest::isolate()->GetCurrentContext())
                   .ToLocalChecked();
  CHECK(heap->InSpace(*v8::Utils::OpenHandle(*result_str), OLD_SPACE));

  result_str = CompileRun("c")
                   ->ToString(CcTest::isolate()->GetCurrentContext())
                   .ToLocalChecked();
  CHECK(heap->InSpace(*v8::Utils::OpenHandle(*result_str), OLD_SPACE));

  delete cache;
  source_a.Dispose();
  source_b.Dispose();
  source_c.Dispose();
}

class SerializerOneByteResource
    : public v8::String::ExternalOneByteStringResource {
 public:
  SerializerOneByteResource(const char* data, size_t length)
      : data_(data), length_(length), dispose_count_(0) {}
  const char* data() const override { return data_; }
  size_t length() const override { return length_; }
  void Dispose() override { dispose_count_++; }
  int dispose_count() { return dispose_count_; }

 private:
  const char* data_;
  size_t length_;
  int dispose_count_;
};

class SerializerTwoByteResource : public v8::String::ExternalStringResource {
 public:
  SerializerTwoByteResource(const char* data, size_t length)
      : data_(AsciiToTwoByteString(data)), length_(length), dispose_count_(0) {}
  ~SerializerTwoByteResource() override { DeleteArray<const uint16_t>(data_); }

  const uint16_t* data() const override { return data_; }
  size_t length() const override { return length_; }
  void Dispose() override { dispose_count_++; }
  int dispose_count() { return dispose_count_; }

 private:
  const uint16_t* data_;
  size_t length_;
  int dispose_count_;
};

TEST(CodeSerializerExternalString) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  // Obtain external internalized one-byte string.
  SerializerOneByteResource one_byte_resource("one_byte", 8);
  Handle<String> one_byte_string =
      isolate->factory()->NewStringFromAsciiChecked("one_byte");
  one_byte_string = isolate->factory()->InternalizeString(one_byte_string);
  one_byte_string->MakeExternal(&one_byte_resource);
  CHECK(one_byte_string->IsExternalOneByteString());
  CHECK(one_byte_string->IsInternalizedString());

  // Obtain external internalized two-byte string.
  SerializerTwoByteResource two_byte_resource("two_byte", 8);
  Handle<String> two_byte_string =
      isolate->factory()->NewStringFromAsciiChecked("two_byte");
  two_byte_string = isolate->factory()->InternalizeString(two_byte_string);
  two_byte_string->MakeExternal(&two_byte_resource);
  CHECK(two_byte_string->IsExternalTwoByteString());
  CHECK(two_byte_string->IsInternalizedString());

  const char* source =
      "var o = {}               \n"
      "o.one_byte = 7;          \n"
      "o.two_byte = 8;          \n"
      "o.one_byte + o.two_byte; \n";
  Handle<String> source_string =
      isolate->factory()
          ->NewStringFromUtf8(base::CStrVector(source))
          .ToHandleChecked();

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig = CompileScriptAndProduceCache(
      isolate, source_string, ScriptDetails(), &cache,
      v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_string, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  CHECK_EQ(15.0, copy_result->Number());

  // This avoids the GC from trying to free stack allocated resources.
  i::Handle<i::ExternalOneByteString>::cast(one_byte_string)
      ->SetResource(isolate, nullptr);
  i::Handle<i::ExternalTwoByteString>::cast(two_byte_string)
      ->SetResource(isolate, nullptr);
  delete cache;
}

TEST(CodeSerializerLargeExternalString) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  Factory* f = isolate->factory();

  v8::HandleScope scope(CcTest::isolate());

  // Create a huge external internalized string to use as variable name.
  base::Vector<const char> string = ConstructSource(
      base::StaticCharVector(""), base::StaticCharVector("abcdef"),
      base::StaticCharVector(""), 999999);
  Handle<String> name = f->NewStringFromUtf8(string).ToHandleChecked();
  SerializerOneByteResource one_byte_resource(
      reinterpret_cast<const char*>(string.begin()), string.length());
  name = f->InternalizeString(name);
  name->MakeExternal(&one_byte_resource);
  CHECK(name->IsExternalOneByteString());
  CHECK(name->IsInternalizedString());
  CHECK(isolate->heap()->InSpace(*name, LO_SPACE));

  // Create the source, which is "var <literal> = 42; <literal>".
  Handle<String> source_str =
      f->NewConsString(
           f->NewConsString(f->NewStringFromAsciiChecked("var "), name)
               .ToHandleChecked(),
           f->NewConsString(f->NewStringFromAsciiChecked(" = 42; "), name)
               .ToHandleChecked())
          .ToHandleChecked();

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig =
      CompileScriptAndProduceCache(isolate, source_str, ScriptDetails(), &cache,
                                   v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_str, ScriptDetails(), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  CHECK_EQ(42.0, copy_result->Number());

  // This avoids the GC from trying to free stack allocated resources.
  i::Handle<i::ExternalOneByteString>::cast(name)->SetResource(isolate,
                                                               nullptr);
  delete cache;
  string.Dispose();
}

TEST(CodeSerializerExternalScriptName) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  Factory* f = isolate->factory();

  v8::HandleScope scope(CcTest::isolate());

  const char* source =
      "var a = [1, 2, 3, 4];"
      "a.reduce(function(x, y) { return x + y }, 0)";

  Handle<String> source_string =
      f->NewStringFromUtf8(base::CStrVector(source)).ToHandleChecked();

  const SerializerOneByteResource one_byte_resource("one_byte", 8);
  Handle<String> name =
      f->NewExternalStringFromOneByte(&one_byte_resource).ToHandleChecked();
  CHECK(name->IsExternalOneByteString());
  CHECK(!name->IsInternalizedString());

  Handle<JSObject> global(isolate->context().global_object(), isolate);
  AlignedCachedData* cache = nullptr;

  Handle<SharedFunctionInfo> orig = CompileScriptAndProduceCache(
      isolate, source_string, ScriptDetails(name), &cache,
      v8::ScriptCompiler::kNoCompileOptions);

  Handle<SharedFunctionInfo> copy;
  {
    DisallowCompilation no_compile_expected(isolate);
    copy = CompileScript(isolate, source_string, ScriptDetails(name), cache,
                         v8::ScriptCompiler::kConsumeCodeCache);
  }
  CHECK_NE(*orig, *copy);

  Handle<JSFunction> copy_fun =
      Factory::JSFunctionBuilder{isolate, copy, isolate->native_context()}
          .Build();

  Handle<Object> copy_result =
      Execution::CallScript(isolate, copy_fun, global,
                            isolate->factory()->empty_fixed_array())
          .ToHandleChecked();

  CHECK_EQ(10.0, copy_result->Number());

  // This avoids the GC from trying to free stack allocated resources.
  i::Handle<i::ExternalOneByteString>::cast(name)->SetResource(isolate,
                                                               nullptr);
  delete cache;
}

static bool toplevel_test_code_event_found = false;

static void SerializerLogEventListener(const v8::JitCodeEvent* event) {
  if (event->type == v8::JitCodeEvent::CODE_ADDED &&
      (memcmp(event->name.str, "Script:~ test", 13) == 0 ||
       memcmp(event->name.str, "Script: test", 12) == 0)) {
    toplevel_test_code_event_found = true;
  }
}

v8::ScriptCompiler::CachedData* CompileRunAndProduceCache(
    const char* js_source, CodeCacheType cacheType = CodeCacheType::kLazy) {
  v8::ScriptCompiler::CachedData* cache;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate1 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate1);
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate1, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin);
    v8::ScriptCompiler::CompileOptions options;
    switch (cacheType) {
      case CodeCacheType::kEager:
        options = v8::ScriptCompiler::kEagerCompile;
        break;
      case CodeCacheType::kLazy:
      case CodeCacheType::kAfterExecute:
        options = v8::ScriptCompiler::kNoCompileOptions;
        break;
      default:
        UNREACHABLE();
    }
    v8::Local<v8::UnboundScript> script =
        v8::ScriptCompiler::CompileUnboundScript(isolate1, &source, options)
            .ToLocalChecked();

    if (cacheType != CodeCacheType::kAfterExecute) {
      cache = ScriptCompiler::CreateCodeCache(script);
    }

    v8::Local<v8::Value> result = script->BindToCurrentContext()
                                      ->Run(isolate1->GetCurrentContext())
                                      .ToLocalChecked();
    v8::Local<v8::String> result_string =
        result->ToString(isolate1->GetCurrentContext()).ToLocalChecked();
    CHECK(result_string->Equals(isolate1->GetCurrentContext(), v8_str("abcdef"))
              .FromJust());

    if (cacheType == CodeCacheType::kAfterExecute) {
      cache = ScriptCompiler::CreateCodeCache(script);
    }
    CHECK(cache);
  }
  isolate1->Dispose();
  return cache;
}

TEST(CodeSerializerIsolates) {
  const char* js_source = "function f() { return 'abc'; }; f() + 'def'";
  v8::ScriptCompiler::CachedData* cache = CompileRunAndProduceCache(js_source);

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  isolate2->SetJitCodeEventHandler(v8::kJitCodeEventDefault,
                                   SerializerLogEventListener);
  toplevel_test_code_event_found = false;
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::Local<v8::UnboundScript> script;
    {
      DisallowCompilation no_compile(reinterpret_cast<Isolate*>(isolate2));
      script = v8::ScriptCompiler::CompileUnboundScript(
                   isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
                   .ToLocalChecked();
    }
    CHECK(!cache->rejected);
    v8::Local<v8::Value> result = script->BindToCurrentContext()
                                      ->Run(isolate2->GetCurrentContext())
                                      .ToLocalChecked();
    CHECK(result->ToString(isolate2->GetCurrentContext())
              .ToLocalChecked()
              ->Equals(isolate2->GetCurrentContext(), v8_str("abcdef"))
              .FromJust());
  }
  CHECK(toplevel_test_code_event_found);
  isolate2->Dispose();
}

TEST(CodeSerializerIsolatesEager) {
  const char* js_source =
      "function f() {"
      "  return function g() {"
      "    return 'abc';"
      "  }"
      "}"
      "f()() + 'def'";
  v8::ScriptCompiler::CachedData* cache =
      CompileRunAndProduceCache(js_source, CodeCacheType::kEager);

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  isolate2->SetJitCodeEventHandler(v8::kJitCodeEventDefault,
                                   SerializerLogEventListener);
  toplevel_test_code_event_found = false;
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::Local<v8::UnboundScript> script;
    {
      DisallowCompilation no_compile(reinterpret_cast<Isolate*>(isolate2));
      script = v8::ScriptCompiler::CompileUnboundScript(
                   isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
                   .ToLocalChecked();
    }
    CHECK(!cache->rejected);
    v8::Local<v8::Value> result = script->BindToCurrentContext()
                                      ->Run(isolate2->GetCurrentContext())
                                      .ToLocalChecked();
    CHECK(result->ToString(isolate2->GetCurrentContext())
              .ToLocalChecked()
              ->Equals(isolate2->GetCurrentContext(), v8_str("abcdef"))
              .FromJust());
  }
  CHECK(toplevel_test_code_event_found);
  isolate2->Dispose();
}

TEST(CodeSerializerAfterExecute) {
  // We test that no compilations happen when running this code. Forcing
  // to always optimize breaks this test.
  bool prev_always_turbofan_value = v8_flags.always_turbofan;
  v8_flags.always_turbofan = false;
  const char* js_source = "function f() { return 'abc'; }; f() + 'def'";
  v8::ScriptCompiler::CachedData* cache =
      CompileRunAndProduceCache(js_source, CodeCacheType::kAfterExecute);

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  Isolate* i_isolate2 = reinterpret_cast<Isolate*>(isolate2);

  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::Local<v8::UnboundScript> script;
    {
      DisallowCompilation no_compile_expected(i_isolate2);
      script = v8::ScriptCompiler::CompileUnboundScript(
                   isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
                   .ToLocalChecked();
    }
    CHECK(!cache->rejected);

    Handle<SharedFunctionInfo> sfi = v8::Utils::OpenHandle(*script);
    CHECK(sfi->HasBytecodeArray());

    {
      DisallowCompilation no_compile_expected(i_isolate2);
      v8::Local<v8::Value> result = script->BindToCurrentContext()
                                        ->Run(isolate2->GetCurrentContext())
                                        .ToLocalChecked();
      v8::Local<v8::String> result_string =
          result->ToString(isolate2->GetCurrentContext()).ToLocalChecked();
      CHECK(
          result_string->Equals(isolate2->GetCurrentContext(), v8_str("abcdef"))
              .FromJust());
    }
  }
  isolate2->Dispose();

  // Restore the flags.
  v8_flags.always_turbofan = prev_always_turbofan_value;
}

TEST(CodeSerializerFlagChange) {
  const char* js_source = "function f() { return 'abc'; }; f() + 'def'";
  v8::ScriptCompiler::CachedData* cache = CompileRunAndProduceCache(js_source);

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);

  v8_flags.allow_natives_syntax =
      true;  // Flag change should trigger cache reject.
  FlagList::EnforceFlagImplications();
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::ScriptCompiler::CompileUnboundScript(
        isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
        .ToLocalChecked();
    CHECK(cache->rejected);
  }
  isolate2->Dispose();
}

TEST(CodeSerializerBitFlip) {
  i::v8_flags.verify_snapshot_checksum = true;
  const char* js_source = "function f() { return 'abc'; }; f() + 'def'";
  v8::ScriptCompiler::CachedData* cache = CompileRunAndProduceCache(js_source);

  // Arbitrary bit flip.
  int arbitrary_spot = 237;
  CHECK_LT(arbitrary_spot, cache->length);
  const_cast<uint8_t*>(cache->data)[arbitrary_spot] ^= 0x40;

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::String> source_str = v8_str(js_source);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::ScriptCompiler::CompileUnboundScript(
        isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
        .ToLocalChecked();
    CHECK(cache->rejected);
  }
  isolate2->Dispose();
}

TEST(CodeSerializerWithHarmonyScoping) {
  const char* source1 = "'use strict'; let x = 'X'";
  const char* source2 = "'use strict'; let y = 'Y'";
  const char* source3 = "'use strict'; x + y";

  v8::ScriptCompiler::CachedData* cache;

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate1 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate1);
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope context_scope(context);

    CompileRun(source1);
    CompileRun(source2);

    v8::Local<v8::String> source_str = v8_str(source3);
    v8::ScriptOrigin origin(isolate1, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin);
    v8::Local<v8::UnboundScript> script =
        v8::ScriptCompiler::CompileUnboundScript(
            isolate1, &source, v8::ScriptCompiler::kNoCompileOptions)
            .ToLocalChecked();
    cache = v8::ScriptCompiler::CreateCodeCache(script);
    CHECK(cache);

    v8::Local<v8::Value> result = script->BindToCurrentContext()
                                      ->Run(isolate1->GetCurrentContext())
                                      .ToLocalChecked();
    v8::Local<v8::String> result_str =
        result->ToString(isolate1->GetCurrentContext()).ToLocalChecked();
    CHECK(result_str->Equals(isolate1->GetCurrentContext(), v8_str("XY"))
              .FromJust());
  }
  isolate1->Dispose();

  v8::Isolate* isolate2 = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context = v8::Context::New(isolate2);
    v8::Context::Scope context_scope(context);

    // Reverse order of prior running scripts.
    CompileRun(source2);
    CompileRun(source1);

    v8::Local<v8::String> source_str = v8_str(source3);
    v8::ScriptOrigin origin(isolate2, v8_str("test"));
    v8::ScriptCompiler::Source source(source_str, origin, cache);
    v8::Local<v8::UnboundScript> script;
    {
      DisallowCompilation no_compile(reinterpret_cast<Isolate*>(isolate2));
      script = v8::ScriptCompiler::CompileUnboundScript(
                   isolate2, &source, v8::ScriptCompiler::kConsumeCodeCache)
                   .ToLocalChecked();
    }
    v8::Local<v8::Value> result = script->BindToCurrentContext()
                                      ->Run(isolate2->GetCurrentContext())
                                      .ToLocalChecked();
    v8::Local<v8::String> result_str =
        result->ToString(isolate2->GetCurrentContext()).ToLocalChecked();
    CHECK(result_str->Equals(isolate2->GetCurrentContext(), v8_str("XY"))
              .FromJust());
  }
  isolate2->Dispose();
}

TEST(Regress503552) {
  if (!v8_flags.incremental_marking) return;
  // Test that the code serializer can deal with weak cells that form a linked
  // list during incremental marking.
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();

  HandleScope scope(isolate);
  Handle<String> source = isolate->factory()->NewStringFromAsciiChecked(
      "function f() {} function g() {}");
  AlignedCachedData* cached_data = nullptr;
  Handle<SharedFunctionInfo> shared = CompileScriptAndProduceCache(
      isolate, source, ScriptDetails(), &cached_data,
      v8::ScriptCompiler::kNoCompileOptions);
  delete cached_data;

  heap::SimulateIncrementalMarking(isolate->heap());

  v8::ScriptCompiler::CachedData* cache_data =
      CodeSerializer::Serialize(isolate, shared);
  delete cache_data;
}

static void CodeSerializerMergeDeserializedScript(bool retain_toplevel_sfi) {
  v8_flags.stress_background_compile = false;
  CcTest::InitializeVM();
  Isolate* isolate = CcTest::i_isolate();

  HandleScope outer_scope(isolate);
  Handle<String> source = isolate->factory()->NewStringFromAsciiChecked(
      "(function () {return 123;})");
  AlignedCachedData* cached_data = nullptr;
  Handle<Script> script;
  {
    HandleScope first_compilation_scope(isolate);
    Handle<SharedFunctionInfo> shared = CompileScriptAndProduceCache(
        isolate, source, ScriptDetails(), &cached_data,
        v8::ScriptCompiler::kNoCompileOptions);
    Handle<BytecodeArray> bytecode =
        handle(shared->GetBytecodeArray(isolate), isolate);
    for (int i = 0; i <= v8_flags.bytecode_old_age; ++i) {
      bytecode->MakeOlder();
    }
    Handle<Script> local_script =
        handle(Script::cast(shared->script()), isolate);
    script = first_compilation_scope.CloseAndEscape(local_script);
  }

  Handle<HeapObject> retained_toplevel_sfi;
  if (retain_toplevel_sfi) {
    retained_toplevel_sfi =
        handle(script->shared_function_infos().Get(0).GetHeapObjectAssumeWeak(),
               isolate);
  }

  // GC twice in case incremental marking had already marked the bytecode array.
  // After this, the Isolate compilation cache contains a weak reference to the
  // Script but not the top-level SharedFunctionInfo.
  CcTest::CollectAllGarbage();
  CcTest::CollectAllGarbage();

  Handle<SharedFunctionInfo> copy =
      CompileScript(isolate, source, ScriptDetails(), cached_data,
                    v8::ScriptCompiler::kConsumeCodeCache);
  delete cached_data;

  // The existing Script was reused.
  CHECK_EQ(*script, copy->script());

  // The existing top-level SharedFunctionInfo was also reused.
  if (retain_toplevel_sfi) {
    CHECK_EQ(*retained_toplevel_sfi, *copy);
  }
}

TEST(CodeSerializerMergeDeserializedScript) {
  CodeSerializerMergeDeserializedScript(/*retain_toplevel_sfi=*/false);
}

TEST(CodeSerializerMergeDeserializedScriptRetainingToplevelSfi) {
  CodeSerializerMergeDeserializedScript(/*retain_toplevel_sfi=*/true);
}

UNINITIALIZED_TEST(SnapshotCreatorBlobNotCreated) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::TryCatch try_catch(isolate);
      v8::Local<v8::String> code = v8_str("throw new Error('test');");
      CHECK(v8::Script::Compile(context, code)
                .ToLocalChecked()
                ->Run(context)
                .IsEmpty());
      CHECK(try_catch.HasCaught());
    }
    // SnapshotCreator should be destroyed just fine even when no
    // blob is created.
  }

  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(SnapshotCreatorMultipleContexts) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun("var f = function() { return 1; }");
      creator.SetDefaultContext(context);
    }
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun("var f = function() { return 2; }");
      CHECK_EQ(0u, creator.AddContext(context));
    }
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      CHECK_EQ(1u, creator.AddContext(context));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &blob;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 1);
    }
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 2);
    }
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 1).ToLocalChecked();
      v8::Context::Scope context_scope(context);
      ExpectUndefined("this.f");
    }
  }

  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

static int serialized_static_field = 314;

static void SerializedCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::ValidateCallbackInfo(info));
  if (info.Data()->IsExternal()) {
    CHECK_EQ(info.Data().As<v8::External>()->Value(),
             static_cast<void*>(&serialized_static_field));
    int* value =
        reinterpret_cast<int*>(info.Data().As<v8::External>()->Value());
    (*value)++;
  }
  info.GetReturnValue().Set(v8_num(42));
}

static void SerializedCallbackReplacement(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::ValidateCallbackInfo(info));
  info.GetReturnValue().Set(v8_num(1337));
}

static void NamedPropertyGetterForSerialization(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(i::ValidateCallbackInfo(info));
  if (name->Equals(info.GetIsolate()->GetCurrentContext(), v8_str("x"))
          .FromJust()) {
    info.GetReturnValue().Set(v8_num(2016));
  }
}

static void AccessorForSerialization(
    v8::Local<v8::String> property,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(i::ValidateCallbackInfo(info));
  info.GetReturnValue().Set(v8_num(2017));
}

static SerializerOneByteResource serializable_one_byte_resource("one_byte", 8);
static SerializerTwoByteResource serializable_two_byte_resource("two_byte", 8);

intptr_t original_external_references[] = {
    reinterpret_cast<intptr_t>(SerializedCallback),
    reinterpret_cast<intptr_t>(&serialized_static_field),
    reinterpret_cast<intptr_t>(&NamedPropertyGetterForSerialization),
    reinterpret_cast<intptr_t>(&AccessorForSerialization),
    reinterpret_cast<intptr_t>(&serialized_static_field),  // duplicate entry
    reinterpret_cast<intptr_t>(&serializable_one_byte_resource),
    reinterpret_cast<intptr_t>(&serializable_two_byte_resource),
    0};

intptr_t replaced_external_references[] = {
    reinterpret_cast<intptr_t>(SerializedCallbackReplacement),
    reinterpret_cast<intptr_t>(&serialized_static_field),
    reinterpret_cast<intptr_t>(&NamedPropertyGetterForSerialization),
    reinterpret_cast<intptr_t>(&AccessorForSerialization),
    reinterpret_cast<intptr_t>(&serialized_static_field),
    reinterpret_cast<intptr_t>(&serializable_one_byte_resource),
    reinterpret_cast<intptr_t>(&serializable_two_byte_resource),
    0};

intptr_t short_external_references[] = {
    reinterpret_cast<intptr_t>(SerializedCallbackReplacement), 0};

UNINITIALIZED_TEST(SnapshotCreatorExternalReferences) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator(original_external_references);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::Local<v8::FunctionTemplate> callback =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      v8::Local<v8::Value> function =
          callback->GetFunction(context).ToLocalChecked();
      CHECK(context->Global()->Set(context, v8_str("f"), function).FromJust());

      CHECK(context->Global()
                ->Set(context, v8_str("one_byte"),
                      v8::String::NewExternalOneByte(
                          isolate, &serializable_one_byte_resource)
                          .ToLocalChecked())
                .FromJust());
      CHECK(context->Global()
                ->Set(context, v8_str("two_byte"),
                      v8::String::NewExternalTwoByte(
                          isolate, &serializable_two_byte_resource)
                          .ToLocalChecked())
                .FromJust());

      ExpectInt32("f()", 42);
      ExpectString("one_byte", "one_byte");
      ExpectString("two_byte", "two_byte");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  CHECK_EQ(1, serializable_one_byte_resource.dispose_count());
  CHECK_EQ(1, serializable_two_byte_resource.dispose_count());

  // Deserialize with the original external reference.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = original_external_references;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 42);
      ExpectString("one_byte", "one_byte");
      ExpectString("two_byte", "two_byte");
      v8::Local<v8::String> one_byte = CompileRun("one_byte").As<v8::String>();
      v8::Local<v8::String> two_byte = CompileRun("two_byte").As<v8::String>();
      CHECK(one_byte->IsExternalOneByte());
      CHECK(!one_byte->IsExternalTwoByte());
      CHECK(!two_byte->IsExternalOneByte());
      CHECK(two_byte->IsExternalTwoByte());
    }
    isolate->Dispose();
  }

  CHECK_EQ(2, serializable_one_byte_resource.dispose_count());
  CHECK_EQ(2, serializable_two_byte_resource.dispose_count());

  // Deserialize with some other external reference.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = replaced_external_references;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 1337);
    }
    isolate->Dispose();
  }

  CHECK_EQ(3, serializable_one_byte_resource.dispose_count());
  CHECK_EQ(3, serializable_two_byte_resource.dispose_count());

  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(SnapshotCreatorShortExternalReferences) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator(original_external_references);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::Local<v8::FunctionTemplate> callback =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      v8::Local<v8::Value> function =
          callback->GetFunction(context).ToLocalChecked();
      CHECK(context->Global()->Set(context, v8_str("f"), function).FromJust());
      ExpectInt32("f()", 42);
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = short_external_references;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 1337);
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

v8::StartupData CreateSnapshotWithDefaultAndCustom() {
  v8::SnapshotCreator creator(original_external_references);
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun("function f() { return 41; }");
      creator.SetDefaultContext(context);
      ExpectInt32("f()", 41);
    }
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::Local<v8::FunctionTemplate> function_template =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      v8::Local<v8::Value> function =
          function_template->GetFunction(context).ToLocalChecked();
      CHECK(context->Global()->Set(context, v8_str("f"), function).FromJust());
      v8::Local<v8::ObjectTemplate> object_template =
          v8::ObjectTemplate::New(isolate);
      object_template->SetAccessor(v8_str("x"), AccessorForSerialization);
      v8::Local<v8::Object> object =
          object_template->NewInstance(context).ToLocalChecked();
      CHECK(context->Global()->Set(context, v8_str("o"), object).FromJust());
      ExpectInt32("f()", 42);
      ExpectInt32("o.x", 2017);
      creator.AddContext(context);
    }
  }
  return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
}

UNINITIALIZED_TEST(SnapshotCreatorNoExternalReferencesDefault) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob = CreateSnapshotWithDefaultAndCustom();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = nullptr;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 41);
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

v8::StartupData CreateCustomSnapshotWithPreparseDataAndNoOuterScope() {
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "var foo = {\n"
          "  // This function is not top-level, but also has no outer scope.\n"
          "  bar: function(){\n"
          "    // Add an inner function so that the outer one has preparse\n"
          "    // scope data.\n"
          "    return function(){}\n"
          "  }\n"
          "};\n");
      creator.SetDefaultContext(context);
    }
  }
  return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
}

UNINITIALIZED_TEST(SnapshotCreatorPreparseDataAndNoOuterScope) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob = CreateCustomSnapshotWithPreparseDataAndNoOuterScope();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

v8::StartupData CreateCustomSnapshotArrayJoinWithKeep() {
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "[].join('');\n"
          "function g() { return String([1,2,3]); }\n");
      ExpectString("g()", "1,2,3");
      creator.SetDefaultContext(context);
    }
  }
  return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);
}

UNINITIALIZED_TEST(SnapshotCreatorArrayJoinWithKeep) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob = CreateCustomSnapshotArrayJoinWithKeep();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectString("g()", "1,2,3");
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

v8::StartupData CreateCustomSnapshotWithDuplicateFunctions() {
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "function f() { return (() => 'a'); }\n"
          "let g1 = f();\n"
          "let g2 = f();\n");
      ExpectString("g1()", "a");
      ExpectString("g2()", "a");
      creator.SetDefaultContext(context);
    }
  }
  return creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);
}

UNINITIALIZED_TEST(SnapshotCreatorDuplicateFunctions) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob = CreateCustomSnapshotWithDuplicateFunctions();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      ExpectString("g1()", "a");
      ExpectString("g2()", "a");
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

#ifndef V8_SHARED_RO_HEAP
// We do not support building multiple snapshots when read-only heap is shared.

TEST(SnapshotCreatorNoExternalReferencesCustomFail1) {
  DisableAlwaysOpt();
  v8::StartupData blob = CreateSnapshotWithDefaultAndCustom();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = nullptr;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 42);
    }
    isolate->Dispose();
  }
  delete[] blob.data;
}

TEST(SnapshotCreatorNoExternalReferencesCustomFail2) {
  DisableAlwaysOpt();
  v8::StartupData blob = CreateSnapshotWithDefaultAndCustom();

  // Deserialize with an incomplete list of external references.
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = nullptr;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Context::Scope context_scope(context);
      ExpectInt32("o.x", 2017);
    }
    isolate->Dispose();
  }
  delete[] blob.data;
}

#endif  // V8_SHARED_RO_HEAP

UNINITIALIZED_TEST(SnapshotCreatorUnknownExternalReferences) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    v8::Local<v8::FunctionTemplate> callback =
        v8::FunctionTemplate::New(isolate, SerializedCallback);
    v8::Local<v8::Value> function =
        callback->GetFunction(context).ToLocalChecked();
    CHECK(context->Global()->Set(context, v8_str("f"), function).FromJust());
    ExpectInt32("f()", 42);

    creator.SetDefaultContext(context);
  }
  v8::StartupData blob =
      creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(SnapshotCreatorTemplates) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;

  {
    InternalFieldData* a1 = new InternalFieldData{11};
    InternalFieldData* b1 = new InternalFieldData{20};
    InternalFieldData* c1 = new InternalFieldData{30};

    v8::SnapshotCreator creator(original_external_references);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::ExtensionConfiguration* no_extension = nullptr;
      v8::Local<v8::ObjectTemplate> global_template =
          v8::ObjectTemplate::New(isolate);
      v8::Local<v8::External> external =
          v8::External::New(isolate, &serialized_static_field);
      v8::Local<v8::FunctionTemplate> callback =
          v8::FunctionTemplate::New(isolate, SerializedCallback, external);
      global_template->Set(isolate, "f", callback);
      v8::Local<v8::Context> context =
          v8::Context::New(isolate, no_extension, global_template);
      creator.SetDefaultContext(context);
      context = v8::Context::New(isolate, no_extension, global_template);
      v8::Local<v8::ObjectTemplate> object_template =
          v8::ObjectTemplate::New(isolate);
      object_template->SetInternalFieldCount(3);

      v8::Context::Scope context_scope(context);
      ExpectInt32("f()", 42);
      CHECK_EQ(315, serialized_static_field);

      v8::Local<v8::Object> a =
          object_template->NewInstance(context).ToLocalChecked();
      v8::Local<v8::Object> b =
          object_template->NewInstance(context).ToLocalChecked();
      v8::Local<v8::Object> c =
          object_template->NewInstance(context).ToLocalChecked();
      v8::Local<v8::External> resource_external =
          v8::External::New(isolate, &serializable_one_byte_resource);
      v8::Local<v8::External> field_external =
          v8::External::New(isolate, &serialized_static_field);

      a->SetInternalField(0, b);
      b->SetInternalField(0, c);

      a->SetAlignedPointerInInternalField(1, a1);
      b->SetAlignedPointerInInternalField(1, b1);
      c->SetAlignedPointerInInternalField(1, c1);

      a->SetInternalField(2, resource_external);
      b->SetInternalField(2, field_external);
      c->SetInternalField(2, v8_num(35));
      CHECK(context->Global()->Set(context, v8_str("a"), a).FromJust());

      CHECK_EQ(0u,
               creator.AddContext(context, v8::SerializeInternalFieldsCallback(
                                               SerializeInternalFields,
                                               reinterpret_cast<void*>(2000))));
      CHECK_EQ(0u, creator.AddData(callback));
      CHECK_EQ(1u, creator.AddData(global_template));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);

    delete a1;
    delete b1;
    delete c1;
  }

  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = original_external_references;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      {
        // Create a new context without a new object template.
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context =
            v8::Context::FromSnapshot(
                isolate, 0,
                v8::DeserializeInternalFieldsCallback(
                    DeserializeInternalFields, reinterpret_cast<void*>(2017)))
                .ToLocalChecked();
        v8::Context::Scope context_scope(context);
        ExpectInt32("f()", 42);
        CHECK_EQ(316, serialized_static_field);

        // Retrieve the snapshotted object template.
        v8::Local<v8::ObjectTemplate> obj_template =
            isolate->GetDataFromSnapshotOnce<v8::ObjectTemplate>(1)
                .ToLocalChecked();
        CHECK(!obj_template.IsEmpty());
        v8::Local<v8::Object> object =
            obj_template->NewInstance(context).ToLocalChecked();
        CHECK(context->Global()->Set(context, v8_str("o"), object).FromJust());
        ExpectInt32("o.f()", 42);
        CHECK_EQ(317, serialized_static_field);
        // Check that it instantiates to the same prototype.
        ExpectTrue("o.f.prototype === f.prototype");

        // Retrieve the snapshotted function template.
        v8::Local<v8::FunctionTemplate> fun_template =
            isolate->GetDataFromSnapshotOnce<v8::FunctionTemplate>(0)
                .ToLocalChecked();
        CHECK(!fun_template.IsEmpty());
        v8::Local<v8::Function> fun =
            fun_template->GetFunction(context).ToLocalChecked();
        CHECK(context->Global()->Set(context, v8_str("g"), fun).FromJust());
        ExpectInt32("g()", 42);
        // Check that it instantiates to the same prototype.
        ExpectTrue("g.prototype === f.prototype");

        // Retrieve embedder fields.
        v8::Local<v8::Object> a = context->Global()
                                      ->Get(context, v8_str("a"))
                                      .ToLocalChecked()
                                      ->ToObject(context)
                                      .ToLocalChecked();
        v8::Local<v8::Object> b =
            a->GetInternalField(0)->ToObject(context).ToLocalChecked();
        v8::Local<v8::Object> c =
            b->GetInternalField(0)->ToObject(context).ToLocalChecked();

        InternalFieldData* a1 = reinterpret_cast<InternalFieldData*>(
            a->GetAlignedPointerFromInternalField(1));
        v8::Local<v8::Value> a2 = a->GetInternalField(2);

        InternalFieldData* b1 = reinterpret_cast<InternalFieldData*>(
            b->GetAlignedPointerFromInternalField(1));
        v8::Local<v8::Value> b2 = b->GetInternalField(2);

        v8::Local<v8::Value> c0 = c->GetInternalField(0);
        InternalFieldData* c1 = reinterpret_cast<InternalFieldData*>(
            c->GetAlignedPointerFromInternalField(1));
        v8::Local<v8::Value> c2 = c->GetInternalField(2);

        CHECK(c0->IsUndefined());

        CHECK_EQ(11u, a1->data);
        CHECK_EQ(20u, b1->data);
        CHECK_EQ(30u, c1->data);

        CHECK(a2->IsExternal());
        CHECK_EQ(static_cast<void*>(&serializable_one_byte_resource),
                 v8::Local<v8::External>::Cast(a2)->Value());
        CHECK(b2->IsExternal());
        CHECK_EQ(static_cast<void*>(&serialized_static_field),
                 v8::Local<v8::External>::Cast(b2)->Value());
        CHECK(c2->IsInt32() && c2->Int32Value(context).FromJust() == 35);

        // Calling GetDataFromSnapshotOnce again returns an empty MaybeLocal.
        CHECK(
            isolate->GetDataFromSnapshotOnce<v8::ObjectTemplate>(1).IsEmpty());
        CHECK(isolate->GetDataFromSnapshotOnce<v8::FunctionTemplate>(0)
                  .IsEmpty());
        CHECK(v8::Context::FromSnapshot(isolate, 1).IsEmpty());

        for (auto data : deserialized_data) delete data;
        deserialized_data.clear();
      }
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

MaybeLocal<v8::Module> ResolveCallback(Local<v8::Context> context,
                                       Local<v8::String> specifier,
                                       Local<v8::FixedArray> import_assertions,
                                       Local<v8::Module> referrer) {
  return {};
}

UNINITIALIZED_TEST(SnapshotCreatorAddData) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;

  // i::PerformCastCheck(Data*) should compile and be no-op
  {
    v8::Local<v8::Data> data;
    i::PerformCastCheck(*data);
  }

  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    v8::Eternal<v8::Value> eternal_number;
    v8::Persistent<v8::Value> persistent_number_1;
    v8::Persistent<v8::Value> persistent_number_2;
    v8::Persistent<v8::Context> persistent_context;
    {
      v8::HandleScope handle_scope(isolate);

      eternal_number.Set(isolate, v8_num(2017));
      persistent_number_1.Reset(isolate, v8_num(2018));
      persistent_number_2.Reset(isolate, v8_num(2019));

      v8::Local<v8::Context> context = v8::Context::New(isolate);
      CHECK_EQ(0u, creator.AddData(context, persistent_number_2.Get(isolate)));
      creator.SetDefaultContext(context);
      context = v8::Context::New(isolate);
      persistent_context.Reset(isolate, context);

      v8::Context::Scope context_scope(context);

      v8::Local<v8::Object> object = CompileRun("({ p: 12 })").As<v8::Object>();

      v8::Local<v8::ObjectTemplate> object_template =
          v8::ObjectTemplate::New(isolate);
      object_template->SetInternalFieldCount(3);

      v8::Local<v8::Private> private_symbol =
          v8::Private::ForApi(isolate, v8_str("private_symbol"));

      v8::Local<v8::Signature> signature =
          v8::Signature::New(isolate, v8::FunctionTemplate::New(isolate));

      v8::ScriptOrigin origin(isolate, v8_str(""), {}, {}, {}, {}, {}, {}, {},
                              true);
      v8::ScriptCompiler::Source source(
          v8::String::NewFromUtf8Literal(
              isolate, "export let a = 42; globalThis.a = {};"),
          origin);
      v8::Local<v8::Module> module =
          v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
      module->InstantiateModule(context, ResolveCallback).ToChecked();
      module->Evaluate(context).ToLocalChecked();

      CHECK_EQ(0u, creator.AddData(context, object));
      CHECK_EQ(1u, creator.AddData(context, v8_str("context-dependent")));
      CHECK_EQ(2u, creator.AddData(context, persistent_number_1.Get(isolate)));
      CHECK_EQ(3u, creator.AddData(context, object_template));
      CHECK_EQ(4u, creator.AddData(context, persistent_context.Get(isolate)));
      CHECK_EQ(5u, creator.AddData(context, module));
      creator.AddContext(context);

      CHECK_EQ(0u, creator.AddData(v8_str("context-independent")));
      CHECK_EQ(1u, creator.AddData(eternal_number.Get(isolate)));
      CHECK_EQ(2u, creator.AddData(object_template));
      CHECK_EQ(3u, creator.AddData(v8::FunctionTemplate::New(isolate)));
      CHECK_EQ(4u, creator.AddData(private_symbol));
      CHECK_EQ(5u, creator.AddData(signature));
    }

    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();

      // Check serialized data on the context.
      v8::Local<v8::Object> object =
          context->GetDataFromSnapshotOnce<v8::Object>(0).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::Object>(0).IsEmpty());
      CHECK_EQ(12, object->Get(context, v8_str("p"))
                       .ToLocalChecked()
                       ->Int32Value(context)
                       .FromJust());

      v8::Local<v8::String> string =
          context->GetDataFromSnapshotOnce<v8::String>(1).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::String>(1).IsEmpty());
      CHECK(string->Equals(context, v8_str("context-dependent")).FromJust());

      v8::Local<v8::Number> number =
          context->GetDataFromSnapshotOnce<v8::Number>(2).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::Number>(2).IsEmpty());
      CHECK_EQ(2018, number->Int32Value(context).FromJust());

      v8::Local<v8::ObjectTemplate> templ =
          context->GetDataFromSnapshotOnce<v8::ObjectTemplate>(3)
              .ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::ObjectTemplate>(3).IsEmpty());
      CHECK_EQ(3, templ->InternalFieldCount());

      v8::Local<v8::Context> serialized_context =
          context->GetDataFromSnapshotOnce<v8::Context>(4).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::Context>(4).IsEmpty());
      CHECK_EQ(*v8::Utils::OpenHandle(*serialized_context),
               *v8::Utils::OpenHandle(*context));

      v8::Local<v8::Module> serialized_module =
          context->GetDataFromSnapshotOnce<v8::Module>(5).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::Context>(5).IsEmpty());
      {
        v8::Context::Scope context_scope(context);
        v8::Local<v8::Object> mod_ns =
            serialized_module->GetModuleNamespace().As<v8::Object>();
        CHECK(mod_ns->Get(context, v8_str("a"))
                  .ToLocalChecked()
                  ->StrictEquals(v8_num(42.0)));
      }

      CHECK(context->GetDataFromSnapshotOnce<v8::Value>(6).IsEmpty());

      // Check serialized data on the isolate.
      string = isolate->GetDataFromSnapshotOnce<v8::String>(0).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::String>(0).IsEmpty());
      CHECK(string->Equals(context, v8_str("context-independent")).FromJust());

      number = isolate->GetDataFromSnapshotOnce<v8::Number>(1).ToLocalChecked();
      CHECK(isolate->GetDataFromSnapshotOnce<v8::Number>(1).IsEmpty());
      CHECK_EQ(2017, number->Int32Value(context).FromJust());

      templ = isolate->GetDataFromSnapshotOnce<v8::ObjectTemplate>(2)
                  .ToLocalChecked();
      CHECK(isolate->GetDataFromSnapshotOnce<v8::ObjectTemplate>(2).IsEmpty());
      CHECK_EQ(3, templ->InternalFieldCount());

      isolate->GetDataFromSnapshotOnce<v8::FunctionTemplate>(3)
          .ToLocalChecked();
      CHECK(
          isolate->GetDataFromSnapshotOnce<v8::FunctionTemplate>(3).IsEmpty());

      isolate->GetDataFromSnapshotOnce<v8::Private>(4).ToLocalChecked();
      CHECK(isolate->GetDataFromSnapshotOnce<v8::Private>(4).IsEmpty());

      isolate->GetDataFromSnapshotOnce<v8::Signature>(5).ToLocalChecked();
      CHECK(isolate->GetDataFromSnapshotOnce<v8::Signature>(5).IsEmpty());

      CHECK(isolate->GetDataFromSnapshotOnce<v8::Value>(7).IsEmpty());
    }
    isolate->Dispose();
  }
  {
    SnapshotCreator creator(nullptr, &blob);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      // Adding data to a snapshot replaces the list of existing data.
      v8::HandleScope hscope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      creator.SetDefaultContext(context);
      context = v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Local<v8::String> string =
          context->GetDataFromSnapshotOnce<v8::String>(1).ToLocalChecked();
      CHECK(context->GetDataFromSnapshotOnce<v8::String>(1).IsEmpty());
      CHECK(string->Equals(context, v8_str("context-dependent")).FromJust());
      v8::Local<v8::Number> number =
          isolate->GetDataFromSnapshotOnce<v8::Number>(1).ToLocalChecked();
      CHECK(isolate->GetDataFromSnapshotOnce<v8::Number>(1).IsEmpty());
      CHECK_EQ(2017, number->Int32Value(context).FromJust());

      CHECK_EQ(0u, creator.AddData(context, v8_num(2016)));
      CHECK_EQ(0u, creator.AddContext(context));
      CHECK_EQ(0u, creator.AddData(v8_str("stuff")));
    }
    delete[] blob.data;
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }
  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);

      // Context where we did not re-add data no longer has data.
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      CHECK(context->GetDataFromSnapshotOnce<v8::Object>(0).IsEmpty());

      // Context where we re-added data has completely new ones.
      context = v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Local<v8::Value> value =
          context->GetDataFromSnapshotOnce<v8::Value>(0).ToLocalChecked();
      CHECK_EQ(2016, value->Int32Value(context).FromJust());
      CHECK(context->GetDataFromSnapshotOnce<v8::Value>(1).IsEmpty());

      // Ditto for the isolate.
      v8::Local<v8::String> string =
          isolate->GetDataFromSnapshotOnce<v8::String>(0).ToLocalChecked();
      CHECK(string->Equals(context, v8_str("stuff")).FromJust());
      CHECK(context->GetDataFromSnapshotOnce<v8::String>(1).IsEmpty());
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

TEST(SnapshotCreatorUnknownHandles) {
  DisableAlwaysOpt();
  v8::StartupData blob;

  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    v8::Eternal<v8::Value> eternal_number;
    v8::Persistent<v8::Value> persistent_number;
    {
      v8::HandleScope handle_scope(isolate);

      eternal_number.Set(isolate, v8_num(2017));
      persistent_number.Reset(isolate, v8_num(2018));

      v8::Local<v8::Context> context = v8::Context::New(isolate);
      creator.SetDefaultContext(context);
    }

    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }
  delete[] blob.data;
}

UNINITIALIZED_TEST(SnapshotAccessorDescriptors) {
  const char* source1 =
      "var bValue = 38;\n"
      "Object.defineProperty(this, 'property1', {\n"
      "    get() { return bValue; },\n"
      "    set(newValue) { bValue = newValue; },\n"
      "});";
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  v8::Isolate* isolate1 = v8::Isolate::New(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::Context> context = v8::Context::New(isolate1);
    v8::Context::Scope c_scope(context);
    ExpectInt32("this.property1", 38);
  }
  isolate1->Dispose();
  delete[] data1.data;
}

UNINITIALIZED_TEST(SnapshotObjectDefinePropertyWhenNewGlobalTemplate) {
  const char* source1 =
      "Object.defineProperty(this, 'property1', {\n"
      "  value: 42,\n"
      "  writable: false\n"
      "});\n"
      "var bValue = 38;\n"
      "Object.defineProperty(this, 'property2', {\n"
      "  get() { return bValue; },\n"
      "  set(newValue) { bValue = newValue; }\n"
      "});";
  v8::StartupData data1 = CreateSnapshotDataBlob(source1);

  v8::Isolate::CreateParams params1;
  params1.snapshot_blob = &data1;
  params1.array_buffer_allocator = CcTest::array_buffer_allocator();

  v8::Isolate* isolate1 = v8::Isolate::New(params1);
  {
    v8::Isolate::Scope i_scope(isolate1);
    v8::HandleScope h_scope(isolate1);
    v8::Local<v8::ObjectTemplate> global_template =
        v8::ObjectTemplate::New(isolate1);
    v8::Local<v8::Context> context =
        v8::Context::New(isolate1, nullptr, global_template);
    v8::Context::Scope c_scope(context);
    ExpectInt32("this.property1", 42);
    ExpectInt32("this.property2", 38);
  }
  isolate1->Dispose();
  delete[] data1.data;
}

UNINITIALIZED_TEST(SnapshotCreatorIncludeGlobalProxy) {
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;

  {
    v8::SnapshotCreator creator(original_external_references);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      // Set default context. This context implicitly does *not* serialize
      // the global proxy, and upon deserialization one has to be created
      // in the bootstrapper from the global object template.
      // Side effects from extensions are persisted though.
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::ObjectTemplate> global_template =
          v8::ObjectTemplate::New(isolate);
      v8::Local<v8::FunctionTemplate> callback =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      global_template->Set(isolate, "f", callback);
      global_template->SetHandler(v8::NamedPropertyHandlerConfiguration(
          NamedPropertyGetterForSerialization));
      v8::Local<v8::Context> context =
          v8::Context::New(isolate, nullptr, global_template);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "function h() { return 13; };"
          "function i() { return 14; };"
          "var o = { p: 7 };");
      ExpectInt32("f()", 42);
      ExpectInt32("h()", 13);
      ExpectInt32("o.p", 7);
      ExpectInt32("x", 2016);
      creator.SetDefaultContext(context);
    }
    {
      // Add additional context. This context implicitly *does* serialize
      // the global proxy, and upon deserialization one has to be created
      // in the bootstrapper from the global object template.
      // Side effects from extensions are persisted.
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::ObjectTemplate> global_template =
          v8::ObjectTemplate::New(isolate);
      v8::Local<v8::FunctionTemplate> callback =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      global_template->SetInternalFieldCount(3);
      global_template->Set(isolate, "f", callback);
      global_template->SetHandler(v8::NamedPropertyHandlerConfiguration(
          NamedPropertyGetterForSerialization));
      global_template->SetAccessor(v8_str("y"), AccessorForSerialization);
      v8::Local<v8::Private> priv =
          v8::Private::ForApi(isolate, v8_str("cached"));
      global_template->SetAccessorProperty(
          v8_str("cached"),
          v8::FunctionTemplate::NewWithCache(isolate, SerializedCallback, priv,
                                             v8::Local<v8::Value>()));
      v8::Local<v8::Context> context =
          v8::Context::New(isolate, nullptr, global_template);
      v8::Context::Scope context_scope(context);

      CHECK(context->Global()
                ->SetPrivate(context, priv, v8_str("cached string"))
                .FromJust());
      v8::Local<v8::Private> hidden =
          v8::Private::ForApi(isolate, v8_str("hidden"));
      CHECK(context->Global()
                ->SetPrivate(context, hidden, v8_str("hidden string"))
                .FromJust());

      ExpectInt32("f()", 42);
      ExpectInt32("x", 2016);
      ExpectInt32("y", 2017);
      CHECK(v8_str("hidden string")
                ->Equals(context, context->Global()
                                      ->GetPrivate(context, hidden)
                                      .ToLocalChecked())
                .FromJust());

      CHECK_EQ(0u,
               creator.AddContext(context, v8::SerializeInternalFieldsCallback(
                                               SerializeInternalFields,
                                               reinterpret_cast<void*>(2016))));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  {
    v8::Isolate::CreateParams params;
    params.snapshot_blob = &blob;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    params.external_references = original_external_references;
    // Test-appropriate equivalent of v8::Isolate::New.
    v8::Isolate* isolate = TestSerializer::NewIsolate(params);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      // We can introduce new extensions, which could override functions already
      // in the snapshot.
      auto extension =
          std::make_unique<v8::Extension>("new extension",
                                          "function i() { return 24; }"
                                          "function j() { return 25; }"
                                          "let a = 26;"
                                          "try {"
                                          "  if (o.p == 7) o.p++;"
                                          "} catch {}");
      extension->set_auto_enable(true);
      v8::RegisterExtension(std::move(extension));
      {
        // Create a new context from default context snapshot. This will also
        // deserialize its global object with interceptor.
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);
        ExpectInt32("f()", 42);
        ExpectInt32("h()", 13);
        ExpectInt32("i()", 24);
        ExpectInt32("j()", 25);
        ExpectInt32("o.p", 8);
        ExpectInt32("a", 26);
        ExpectInt32("x", 2016);
      }
      {
        // Create a new context from first additional context snapshot. This
        // will use the global object from the snapshot, including interceptor.
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context =
            v8::Context::FromSnapshot(
                isolate, 0,
                v8::DeserializeInternalFieldsCallback(
                    DeserializeInternalFields, reinterpret_cast<void*>(2017)))
                .ToLocalChecked();

        {
          v8::Context::Scope context_scope(context);
          ExpectInt32("f()", 42);
          ExpectInt32("i()", 24);
          ExpectInt32("j()", 25);
          ExpectInt32("x", 2016);
          v8::Local<v8::Private> hidden =
              v8::Private::ForApi(isolate, v8_str("hidden"));
          CHECK(v8_str("hidden string")
                    ->Equals(context, context->Global()
                                          ->GetPrivate(context, hidden)
                                          .ToLocalChecked())
                    .FromJust());
          ExpectString("cached", "cached string");
        }

        v8::Local<v8::Object> global = context->Global();
        CHECK_EQ(3, global->InternalFieldCount());
        context->DetachGlobal();

        // New context, but reuse global proxy.
        v8::ExtensionConfiguration* no_extensions = nullptr;
        v8::Local<v8::Context> context2 =
            v8::Context::FromSnapshot(
                isolate, 0,
                v8::DeserializeInternalFieldsCallback(
                    DeserializeInternalFields, reinterpret_cast<void*>(2017)),
                no_extensions, global)
                .ToLocalChecked();
        {
          v8::Context::Scope context_scope(context2);
          ExpectInt32("f()", 42);
          ExpectInt32("i()", 24);
          ExpectInt32("j()", 25);
          ExpectInt32("x", 2016);
          v8::Local<v8::Private> hidden =
              v8::Private::ForApi(isolate, v8_str("hidden"));
          CHECK(v8_str("hidden string")
                    ->Equals(context2, context2->Global()
                                           ->GetPrivate(context2, hidden)
                                           .ToLocalChecked())
                    .FromJust());

          // Set cached accessor property again.
          v8::Local<v8::Private> priv =
              v8::Private::ForApi(isolate, v8_str("cached"));
          CHECK(context2->Global()
                    ->SetPrivate(context2, priv, v8_str("cached string 1"))
                    .FromJust());
          ExpectString("cached", "cached string 1");
        }

        CHECK(context2->Global()->Equals(context2, global).FromJust());
      }
    }
    isolate->Dispose();
  }
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ReinitializeHashSeedJSCollectionRehashable) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      // Create an object with an ordered hash table.
      CompileRun(
          "var m = new Map();"
          "m.set('a', 1);"
          "m.set('b', 2);"
          "var s = new Set();"
          "s.add(1);"
          "s.add(globalThis);");
      ExpectInt32("m.get('b')", 2);
      ExpectTrue("s.has(1)");
      ExpectTrue("s.has(globalThis)");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
    CHECK(blob.CanBeRehashed());
  }

  i::v8_flags.hash_seed = 1337;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    // Check that rehashing has been performed.
    CHECK_EQ(static_cast<uint64_t>(1337),
             HashSeed(reinterpret_cast<i::Isolate*>(isolate)));
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectInt32("m.get('b')", 2);
    ExpectTrue("s.has(1)");
    ExpectTrue("s.has(globalThis)");
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ReinitializeHashSeedRehashable) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      // Create dictionary mode object.
      CompileRun(
          "var a = new Array(10000);"
          "%NormalizeElements(a);"
          "a[133] = 1;"
          "a[177] = 2;"
          "a[971] = 3;"
          "a[7997] = 4;"
          "a[2111] = 5;"
          "var o = {};"
          "%OptimizeObjectForAddingMultipleProperties(o, 3);"
          "o.a = 1;"
          "o.b = 2;"
          "o.c = 3;"
          "var p = { foo: 1 };"  // Test rehashing of transition arrays.
          "p = JSON.parse('{\"foo\": {\"x\": 1}}');");
      i::Handle<i::Object> i_a = v8::Utils::OpenHandle(*CompileRun("a"));
      i::Handle<i::Object> i_o = v8::Utils::OpenHandle(*CompileRun("o"));
      CHECK(i_a->IsJSArray());
      CHECK(i_a->IsJSObject());
      CHECK(!i::Handle<i::JSArray>::cast(i_a)->HasFastElements());
      CHECK(!i::Handle<i::JSObject>::cast(i_o)->HasFastProperties());
      ExpectInt32("a[2111]", 5);
      ExpectInt32("o.c", 3);
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
    CHECK(blob.CanBeRehashed());
  }

  i::v8_flags.hash_seed = 1337;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    // Check that rehashing has been performed.
    CHECK_EQ(static_cast<uint64_t>(1337),
             HashSeed(reinterpret_cast<i::Isolate*>(isolate)));
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    i::Handle<i::Object> i_a = v8::Utils::OpenHandle(*CompileRun("a"));
    i::Handle<i::Object> i_o = v8::Utils::OpenHandle(*CompileRun("o"));
    CHECK(i_a->IsJSArray());
    CHECK(i_a->IsJSObject());
    CHECK(!i::Handle<i::JSArray>::cast(i_a)->HasFastElements());
    CHECK(!i::Handle<i::JSObject>::cast(i_o)->HasFastProperties());
    ExpectInt32("a[2111]", 5);
    ExpectInt32("o.c", 3);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFields) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class ClassWithFieldInitializer {"
          "  #field = 1;"
          "  constructor(val) {"
          "    this.#field = val;"
          "  }"
          "  get field() {"
          "    return this.#field;"
          "  }"
          "}"
          "class ClassWithDefaultConstructor {"
          "  #field = 42;"
          "  get field() {"
          "    return this.#field;"
          "  }"
          "}"
          "class ClassWithFieldDeclaration {"
          "  #field;"
          "  constructor(val) {"
          "    this.#field = val;"
          "  }"
          "  get field() {"
          "    return this.#field;"
          "  }"
          "}"
          "class ClassWithPublicField {"
          "  field = 1;"
          "  constructor(val) {"
          "    this.field = val;"
          "  }"
          "}"
          "class ClassWithFunctionField {"
          "  field = 123;"
          "  func = () => { return this.field; }"
          "}"
          "class ClassWithThisInInitializer {"
          "  #field = 123;"
          "  field = this.#field;"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectInt32("(new ClassWithFieldInitializer(123)).field", 123);
    ExpectInt32("(new ClassWithDefaultConstructor()).field", 42);
    ExpectInt32("(new ClassWithFieldDeclaration(123)).field", 123);
    ExpectInt32("(new ClassWithPublicField(123)).field", 123);
    ExpectInt32("(new ClassWithFunctionField()).func()", 123);
    ExpectInt32("(new ClassWithThisInInitializer()).field", 123);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsReferencePrivateInInitializer) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class A {"
          "  #a = 42;"
          "  a = this.#a;"
          "}"
          "let str;"
          "class ClassWithEval {"
          "  field = eval(str);"
          "}"
          "class ClassWithPrivateAndEval {"
          "  #field = 42;"
          "  field = eval(str);"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectInt32("(new A()).a", 42);
    v8::TryCatch try_catch(isolate);
    CompileRun("str = 'this.#nonexistent'; (new ClassWithEval()).field");
    CHECK(try_catch.HasCaught());
    ExpectInt32("str = 'this.#field'; (new ClassWithPrivateAndEval()).field",
                42);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsReferenceClassVariable) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class PrivateFieldClass {"
          "  #consturctor = PrivateFieldClass;"
          "  func() {"
          "    return this.#consturctor;"
          "  }"
          "}"
          "class PublicFieldClass {"
          "  ctor = PublicFieldClass;"
          "  func() {"
          "    return this.ctor;"
          "  }"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectTrue("new PrivateFieldClass().func() === PrivateFieldClass");
    ExpectTrue("new PublicFieldClass().func() === PublicFieldClass");
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsNested) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class Outer {"
          "  #odata = 42;"
          "  #inner;"
          "  static getInner() {"
          "    class Inner {"
          "      #idata = 42;"
          "      #outer;"
          "      constructor(outer) {"
          "        this.#outer = outer;"
          "        outer.#inner = this;"
          "      }"
          "      check() {"
          "        return this.#idata === this.#outer.#odata &&"
          "               this === this.#outer.#inner;"
          "      }"
          "    }"
          "    return Inner;"
          "  }"
          "  check() {"
          "    return this.#inner.check();"
          "  }"
          "}"
          "const Inner = Outer.getInner();");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectTrue("(new Inner(new Outer)).check()");
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassPrivateMethods) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class JustPrivateMethods {"
          "  #method() { return this.val; }"
          "  get #accessor() { return this.val; };"
          "  set #accessor(val) { this.val = val; }"
          "  method() { return this.#method(); } "
          "  getter() { return this.#accessor; } "
          "  setter(val) { this.#accessor = val } "
          "}"
          "class PrivateMethodsAndFields {"
          "  #val = 1;"
          "  #method() { return this.#val; }"
          "  get #accessor() { return this.#val; };"
          "  set #accessor(val) { this.#val = val; }"
          "  method() { return this.#method(); } "
          "  getter() { return this.#accessor; } "
          "  setter(val) { this.#accessor = val } "
          "}"
          "class Nested {"
          "  #val = 42;"
          "  static #method(obj) { return obj.#val; }"
          "  getInner() {"
          "    class Inner {"
          "      runEval(obj, str) {"
          "        return eval(str);"
          "      }"
          "    }"
          "    return Inner;"
          "  }"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    CompileRun("const a = new JustPrivateMethods(); a.setter(42);");
    ExpectInt32("a.method()", 42);
    ExpectInt32("a.getter()", 42);
    CompileRun("const b = new PrivateMethodsAndFields(); b.setter(42);");
    ExpectInt32("b.method()", 42);
    ExpectInt32("b.getter()", 42);
    CompileRun("const c = new (new Nested().getInner());");
    ExpectInt32("c.runEval(new Nested(), 'Nested.#method(obj)')", 42);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsWithInheritance) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "class Base {"
          "    #a = 'test';"
          "    getA() { return this.#a; }"
          "}"
          "class Derived extends Base {"
          "  #b = 1;"
          "  constructor() {"
          "      super();"
          "      this.#b = this.getA();"
          "  }"
          "  check() {"
          "    return this.#b === this.getA();"
          "  }"
          "}"
          "class DerivedDefaultConstructor extends Base {"
          "  #b = 1;"
          "  check() {"
          "    return this.#b === 1;"
          "  }"
          "}"
          "class NestedSuper extends Base {"
          "  #b = 1;"
          "  constructor() {"
          "    const t = () => {"
          "      super();"
          "    };"
          "    t();"
          "  }"
          "  check() {"
          "    return this.#b === 1;"
          "  }"
          "}"
          "class EvaledSuper extends Base {"
          "  #b = 1;"
          "  constructor() {"
          "    eval('super()');"
          "  }"
          "  check() {"
          "    return this.#b === 1;"
          "  }"
          "}"
          "class NestedEvaledSuper extends Base {"
          "  #b = 1;"
          "  constructor() {"
          "    const t = () => {"
          "      eval('super()');"
          "    };"
          "    t();"
          "  }"
          "  check() {"
          "    return this.#b === 1;"
          "  }"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectBoolean("(new Derived()).check()", true);
    ExpectBoolean("(new DerivedDefaultConstructor()).check()", true);
    ExpectBoolean("(new NestedSuper()).check()", true);
    ExpectBoolean("(new EvaledSuper()).check()", true);
    ExpectBoolean("(new NestedEvaledSuper()).check()", true);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsRecalcPrivateNames) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "let heritageFn;"
          "class Outer {"
          "  #f = 'Outer.#f';"
          "  static Inner = class Inner extends (heritageFn = function () {"
          "               return class Nested {"
          "                 exfil(obj) { return obj.#f; }"
          "                 exfilEval(obj) { return eval('obj.#f'); }"
          "               };"
          "             }) {"
          "               #f = 'Inner.#f';"
          "             };"
          "};");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    CompileRun(
        "const o = new Outer;"
        "const c = new Outer.Inner;"
        "const D = heritageFn();"
        "const d = new D;"
        "let error1;"
        "let error2;");
    ExpectString("d.exfil(o)", "Outer.#f");
    ExpectString("d.exfilEval(o)", "Outer.#f");
    CompileRun("try { d.exfil(c) } catch(e) { error1 = e; }");
    ExpectBoolean("error1 instanceof TypeError", true);
    CompileRun("try { d.exfilEval(c) } catch(e) { error2 = e; }");
    ExpectBoolean("error2 instanceof TypeError", true);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

UNINITIALIZED_TEST(ClassFieldsWithBindings) {
  DisableAlwaysOpt();
  i::v8_flags.rehash_snapshot = true;
  i::v8_flags.hash_seed = 42;
  i::v8_flags.allow_natives_syntax = true;
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "function testVarBinding() {"
          "  function FuncWithVar() {"
          "    this.getPrivate = () => 'test';"
          "  }"
          "  class Derived extends FuncWithVar {"
          "    ['computed'] = FuncWithVar;"
          "    #private = FuncWithVar;"
          "    public = FuncWithVar;"
          "    constructor() {"
          "        super();"
          "        this.#private = this.getPrivate();"
          "    }"
          "    check() {"
          "      return this.#private === this.getPrivate() &&"
          "             this.computed === FuncWithVar &&"
          "             this.public === FuncWithVar;"
          "    }"
          "  }"
          ""
          "  return((new Derived()).check());"
          "}"
          "class ClassWithLet {"
          "    #private = 'test';"
          "    getPrivate() { return this.#private; }"
          "}"
          "function testLetBinding() {"
          "  class Derived extends ClassWithLet {"
          "    ['computed'] = ClassWithLet;"
          "    #private = ClassWithLet;"
          "    public = ClassWithLet;"
          "    constructor() {"
          "        super();"
          "        this.#private = this.getPrivate();"
          "    }"
          "    check() {"
          "      return this.#private === this.getPrivate() &&"
          "             this.computed === ClassWithLet &&"
          "             this.public === ClassWithLet;"
          "    }"
          "  }"
          ""
          "  return((new Derived()).check());"
          "}"
          "const ClassWithConst = class {"
          "    #private = 'test';"
          "    getPrivate() { return this.#private; }"
          "};"
          "function testConstBinding() {"
          "  class Derived extends ClassWithConst {"
          "    ['computed'] = ClassWithConst;"
          "    #private = ClassWithConst;"
          "    public = ClassWithConst;"
          "    constructor() {"
          "        super();"
          "        this.#private = this.getPrivate();"
          "    }"
          "    check() {"
          "      return this.#private === this.getPrivate() &&"
          "             this.computed === ClassWithConst &&"
          "             this.public === ClassWithConst;"
          "    }"
          "  }"
          ""
          "  return((new Derived()).check());"
          "}");
      creator.SetDefaultContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  create_params.snapshot_blob = &blob;
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    CHECK(!context.IsEmpty());
    v8::Context::Scope context_scope(context);
    ExpectBoolean("testVarBinding()", true);
    ExpectBoolean("testLetBinding()", true);
    ExpectBoolean("testConstBinding()", true);
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

void CheckSFIsAreWeak(WeakFixedArray sfis, Isolate* isolate) {
  CHECK_GT(sfis.length(), 0);
  int no_of_weak = 0;
  for (int i = 0; i < sfis.length(); ++i) {
    MaybeObject maybe_object = sfis.Get(i);
    HeapObject heap_object;
    CHECK(maybe_object->IsWeakOrCleared() ||
          (maybe_object->GetHeapObjectIfStrong(&heap_object) &&
           heap_object.IsUndefined(isolate)));
    if (maybe_object->IsWeak()) {
      ++no_of_weak;
    }
  }
  CHECK_GT(no_of_weak, 0);
}

UNINITIALIZED_TEST(WeakArraySerializationInSnapshot) {
  const char* code = "var my_func = function() { }";

  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  i::v8_flags.allow_natives_syntax = true;
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);

      CompileRun(code);
      creator.SetDefaultContext(
          context, v8::SerializeInternalFieldsCallback(
                       SerializeInternalFields, reinterpret_cast<void*>(2016)));
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams create_params;
  create_params.snapshot_blob = &blob;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = TestSerializer::NewIsolate(create_params);
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope h_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(
        isolate, nullptr, v8::MaybeLocal<v8::ObjectTemplate>(),
        v8::MaybeLocal<v8::Value>(),
        v8::DeserializeInternalFieldsCallback(DeserializeInternalFields,
                                              reinterpret_cast<void*>(2017)));
    v8::Context::Scope c_scope(context);

    v8::Local<v8::Value> x = CompileRun("my_func");
    CHECK(x->IsFunction());
    Handle<JSFunction> function =
        Handle<JSFunction>::cast(v8::Utils::OpenHandle(*x));

    // Verify that the pointers in shared_function_infos are weak.
    WeakFixedArray sfis =
        Script::cast(function->shared().script()).shared_function_infos();
    CheckSFIsAreWeak(sfis, reinterpret_cast<i::Isolate*>(isolate));
  }
  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

TEST(WeakArraySerializationInCodeCache) {
  LocalContext context;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()->DisableScriptAndEval();

  v8::HandleScope scope(CcTest::isolate());

  const char* source = "function foo() { }";

  Handle<String> src = isolate->factory()
                           ->NewStringFromUtf8(base::CStrVector(source))
                           .ToHandleChecked();
  AlignedCachedData* cache = nullptr;

  ScriptDetails script_details(src);
  CompileScriptAndProduceCache(isolate, src, script_details, &cache,
                               v8::ScriptCompiler::kNoCompileOptions);

  DisallowCompilation no_compile_expected(isolate);
  Handle<SharedFunctionInfo> copy =
      CompileScript(isolate, src, script_details, cache,
                    v8::ScriptCompiler::kConsumeCodeCache);

  // Verify that the pointers in shared_function_infos are weak.
  WeakFixedArray sfis = Script::cast(copy->script()).shared_function_infos();
  CheckSFIsAreWeak(sfis, isolate);

  delete cache;
}

TEST(CachedCompileFunction) {
  DisableAlwaysOpt();
  LocalContext env;
  Isolate* isolate = CcTest::i_isolate();
  isolate->compilation_cache()
      ->DisableScriptAndEval();  // Disable same-isolate code cache.

  v8::HandleScope scope(CcTest::isolate());

  v8::Local<v8::String> source = v8_str("return x*x;");
  v8::Local<v8::String> arg_str = v8_str("x");
  ScriptCompiler::CachedData* cache;
  {
    v8::ScriptCompiler::Source script_source(source);
    v8::Local<v8::Function> fun =
        v8::ScriptCompiler::CompileFunction(env.local(), &script_source, 1,
                                            &arg_str, 0, nullptr,
                                            v8::ScriptCompiler::kEagerCompile)
            .ToLocalChecked();
    cache = v8::ScriptCompiler::CreateCodeCacheForFunction(fun);
  }

  {
    DisallowCompilation no_compile_expected(isolate);
    v8::ScriptCompiler::Source script_source(source, cache);
    v8::Local<v8::Function> fun =
        v8::ScriptCompiler::CompileFunction(
            env.local(), &script_source, 1, &arg_str, 0, nullptr,
            v8::ScriptCompiler::kConsumeCodeCache)
            .ToLocalChecked();
    v8::Local<v8::Value> arg = v8_num(3);
    v8::Local<v8::Value> result =
        fun->Call(env.local(), v8::Undefined(CcTest::isolate()), 1, &arg)
            .ToLocalChecked();
    CHECK_EQ(9, result->Int32Value(env.local()).FromJust());
  }
}

UNINITIALIZED_TEST(SnapshotCreatorAnonClassWithKeep) {
  DisableAlwaysOpt();
  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "function Foo() { return class {}; } \n"
          "class Bar extends Foo() {}\n"
          "Foo()\n");
      creator.SetDefaultContext(context);
    }
  }
  v8::StartupData blob =
      creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);

  delete[] blob.data;
}

UNINITIALIZED_TEST(SnapshotCreatorDontDeferByteArrayForTypedArray) {
  DisableAlwaysOpt();
  v8::StartupData blob;
  {
    v8::SnapshotCreator creator;
    v8::Isolate* isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);

      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "const z = new Uint8Array(1);\n"
          "class A { \n"
          "  static x() { \n"
          "  } \n"
          "} \n"
          "class B extends A {} \n"
          "B.foo = ''; \n"
          "class C extends B {} \n"
          "class D extends C {} \n"
          "class E extends B {} \n"
          "function F() {} \n"
          "Object.setPrototypeOf(F, D); \n");
      creator.SetDefaultContext(context);
    }

    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
    CHECK(blob.raw_size > 0 && blob.data != nullptr);
  }
  {
    SnapshotCreator creator(nullptr, &blob);
    v8::Isolate* isolate = creator.GetIsolate();
    v8::HandleScope scope(isolate);
    USE(v8::Context::New(isolate));
  }
  delete[] blob.data;
}

class V8_NODISCARD DisableLazySourcePositionScope {
 public:
  DisableLazySourcePositionScope()
      : backup_value_(v8_flags.enable_lazy_source_positions) {
    v8_flags.enable_lazy_source_positions = false;
  }
  ~DisableLazySourcePositionScope() {
    v8_flags.enable_lazy_source_positions = backup_value_;
  }

 private:
  bool backup_value_;
};

UNINITIALIZED_TEST(NoStackFrameCacheSerialization) {
  // Checks that exceptions caught are not cached in the
  // stack frame cache during serialization. The individual frames
  // can point to JSFunction objects, which need to be stored in a
  // context snapshot, *not* isolate snapshot.
  DisableAlwaysOpt();
  DisableLazySourcePositionScope lazy_scope;

  v8::SnapshotCreator creator;
  v8::Isolate* isolate = creator.GetIsolate();
  isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  {
    v8::HandleScope handle_scope(isolate);
    {
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::TryCatch try_catch(isolate);
      CompileRun(R"(
        function foo() { throw new Error('bar'); }
        function bar() {
          foo();
        }
        bar();
      )");

      creator.SetDefaultContext(context);
    }
  }
  v8::StartupData blob =
      creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kKeep);

  delete[] blob.data;
}

namespace {
void CheckObjectsAreInSharedHeap(Isolate* isolate) {
  Heap* heap = isolate->heap();
  HeapObjectIterator iterator(heap);
  DisallowGarbageCollection no_gc;
  for (HeapObject obj = iterator.Next(); !obj.is_null();
       obj = iterator.Next()) {
    const bool expected_in_shared_old =
        heap->MustBeInSharedOldSpace(obj) ||
        (obj.IsString() && String::IsInPlaceInternalizable(String::cast(obj)));
    if (expected_in_shared_old) {
      CHECK(obj.InAnySharedSpace());
    }
  }
}
}  // namespace

UNINITIALIZED_TEST(SharedStrings) {
  // Test that deserializing with --shared-string-table deserializes into the
  // shared Isolate.

  if (!V8_CAN_CREATE_SHARED_HEAP_BOOL) return;

  // Make all the flags that require a shared heap false before creating the
  // isolate to serialize.
  v8_flags.shared_string_table = false;
  v8_flags.harmony_struct = false;

  v8::Isolate* isolate_to_serialize = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs = Serialize(isolate_to_serialize);
  isolate_to_serialize->Dispose();

  v8_flags.shared_string_table = true;

  v8::Isolate* isolate1 = TestSerializer::NewIsolateFromBlob(blobs);
  v8::Isolate* isolate2 = TestSerializer::NewIsolateFromBlob(blobs);
  Isolate* i_isolate1 = reinterpret_cast<Isolate*>(isolate1);
  Isolate* i_isolate2 = reinterpret_cast<Isolate*>(isolate2);

  CHECK_EQ(i_isolate1->string_table(), i_isolate2->string_table());
  {
    ParkedScope parked(i_isolate2->main_thread_local_heap());
    CheckObjectsAreInSharedHeap(i_isolate1);
  }

  {
    ParkedScope parked(i_isolate1->main_thread_local_heap());
    CheckObjectsAreInSharedHeap(i_isolate2);
  }

  {
    // Because both isolate1 and isolate2 are considered running on the main
    // thread, one must be parked to avoid deadlock in the shared heap
    // verification that may happen on client heap disposal.
    ParkedScope parked(i_isolate1->main_thread_local_isolate());
    isolate2->Dispose();
  }
  isolate1->Dispose();

  blobs.Dispose();
  FreeCurrentEmbeddedBlob();
}

namespace {

class DebugBreakCounter : public v8::debug::DebugDelegate {
 public:
  void BreakProgramRequested(v8::Local<v8::Context>,
                             const std::vector<v8::debug::BreakpointId>&,
                             v8::debug::BreakReasons break_reasons) override {
    break_point_hit_count_++;
  }

  int break_point_hit_count() const { return break_point_hit_count_; }

 private:
  int break_point_hit_count_ = 0;
};

}  // namespace

UNINITIALIZED_TEST(BreakPointAccessorContextSnapshot) {
  // Tests that a breakpoint set in one deserialized context also gets hit in
  // another for lazy accessors.
  DisableAlwaysOpt();
  DisableEmbeddedBlobRefcounting();
  v8::StartupData blob;

  {
    v8::SnapshotCreator creator(original_external_references);
    v8::Isolate* isolate = creator.GetIsolate();
    {
      // Add a context to the snapshot that adds an object with an accessor to
      // the global template.
      v8::HandleScope scope(isolate);

      auto accessor_tmpl =
          v8::FunctionTemplate::New(isolate, SerializedCallback);
      accessor_tmpl->SetClassName(v8_str("get f"));
      auto object_tmpl = v8::ObjectTemplate::New(isolate);
      object_tmpl->SetAccessorProperty(v8_str("f"), accessor_tmpl);

      auto global_tmpl = v8::ObjectTemplate::New(isolate);
      global_tmpl->Set(v8_str("o"), object_tmpl);

      creator.SetDefaultContext(v8::Context::New(isolate));

      v8::Local<v8::Context> context =
          v8::Context::New(isolate, nullptr, global_tmpl);
      creator.AddContext(context);
    }
    blob =
        creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
  }

  v8::Isolate::CreateParams params;
  params.snapshot_blob = &blob;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  params.external_references = original_external_references;
  // Test-appropriate equivalent of v8::Isolate::New.
  v8::Isolate* isolate = TestSerializer::NewIsolate(params);
  {
    v8::Isolate::Scope isolate_scope(isolate);

    DebugBreakCounter delegate;
    v8::debug::SetDebugDelegate(isolate, &delegate);

    {
      // Create a new context from the snapshot, put a breakpoint on the
      // accessor and make sure we hit the breakpoint.
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Context::Scope context_scope(context);

      // 1. Set the breakpoint
      v8::Local<v8::Function> function =
          CompileRun(context, "Object.getOwnPropertyDescriptor(o, 'f').get")
              .ToLocalChecked()
              .As<v8::Function>();
      debug::BreakpointId id;
      debug::SetFunctionBreakpoint(function, v8::Local<v8::String>(), &id);

      // 2. Run and check that we hit the breakpoint
      CompileRun(context, "o.f");
      CHECK_EQ(1, delegate.break_point_hit_count());
    }

    {
      // Create a second context from the snapshot and make sure we still hit
      // the breakpoint without setting it again.
      v8::HandleScope scope(isolate);
      v8::Local<v8::Context> context =
          v8::Context::FromSnapshot(isolate, 0).ToLocalChecked();
      v8::Context::Scope context_scope(context);

      CompileRun(context, "o.f");
      CHECK_EQ(2, delegate.break_point_hit_count());
    }

    v8::debug::SetDebugDelegate(isolate, nullptr);
  }

  isolate->Dispose();
  delete[] blob.data;
  FreeCurrentEmbeddedBlob();
}

// These two flags are preconditions for static roots to work. We don't check
// for V8_STATIC_ROOTS_BOOL since the test targets mksnapshot built without
// static roots, to be able to generate the static-roots.h file.
#if defined(V8_COMPRESS_POINTERS_IN_SHARED_CAGE) && defined(V8_SHARED_RO_HEAP)
UNINITIALIZED_TEST(StaticRootsPredictableSnapshot) {
  if (v8_flags.random_seed == 0) {
    return;
  }

  v8::Isolate* isolate1 = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs1 = Serialize(isolate1);
  isolate1->Dispose();

  v8::Isolate* isolate2 = TestSerializer::NewIsolateInitialized();
  StartupBlobs blobs2 = Serialize(isolate2);
  isolate2->Dispose();

  // We want to ensure that setup-heap-internal.cc creates a predictable heap.
  // For static roots it would be sufficient to check that the root pointers
  // relative to the cage base are identical. However, we can't test this, since
  // when we create two isolates in the same process, the offsets will actually
  // be different.
  CHECK_EQ(blobs1.read_only, blobs2.read_only);

  blobs1.Dispose();
  blobs2.Dispose();
  FreeCurrentEmbeddedBlob();
}
#endif  // defined(V8_COMPRESS_POINTERS_IN_SHARED_CAGE) &&
        // defined(V8_SHARED_RO_HEAP)

}  // namespace internal
}  // namespace v8
