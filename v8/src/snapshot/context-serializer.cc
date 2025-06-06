// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/context-serializer.h"
#include "src/snapshot/startup-serializer.h"

#include "src/api/api-inl.h"
#include "src/execution/microtask-queue.h"
#include "src/heap/combined-heap.h"
#include "src/numbers/math-random.h"
#include "src/objects/objects-inl.h"
#include "src/objects/slots.h"

namespace v8 {
namespace internal {

namespace {

// During serialization, puts the native context into a state understood by the
// serializer (e.g. by clearing lists of InstructionStream objects).  After
// serialization, the original state is restored.
class V8_NODISCARD SanitizeNativeContextScope final {
 public:
  SanitizeNativeContextScope(Isolate* isolate, NativeContext native_context,
                             bool allow_active_isolate_for_testing,
                             const DisallowGarbageCollection& no_gc)
      : native_context_(native_context), no_gc_(no_gc) {
#ifdef DEBUG
    if (!allow_active_isolate_for_testing) {
      // Microtasks.
      MicrotaskQueue* microtask_queue = native_context_.microtask_queue();
      DCHECK_EQ(0, microtask_queue->size());
      DCHECK(!microtask_queue->HasMicrotasksSuppressions());
      DCHECK_EQ(0, microtask_queue->GetMicrotasksScopeDepth());
      DCHECK(microtask_queue->DebugMicrotasksScopeDepthIsZero());
    }
#endif
    microtask_queue_external_pointer_ =
        native_context
            .RawExternalPointerField(NativeContext::kMicrotaskQueueOffset)
            .GetAndClearContentForSerialization(no_gc);
  }

  ~SanitizeNativeContextScope() {
    // Restore saved fields.
    native_context_
        .RawExternalPointerField(NativeContext::kMicrotaskQueueOffset)
        .RestoreContentAfterSerialization(microtask_queue_external_pointer_,
                                          no_gc_);
  }

 private:
  NativeContext native_context_;
  ExternalPointerSlot::RawContent microtask_queue_external_pointer_;
  const DisallowGarbageCollection& no_gc_;
};

}  // namespace

ContextSerializer::ContextSerializer(
    Isolate* isolate, Snapshot::SerializerFlags flags,
    StartupSerializer* startup_serializer,
    v8::SerializeEmbedderFieldsCallback callback)
    : Serializer(isolate, flags),
      startup_serializer_(startup_serializer),
      serialize_embedder_fields_(callback),
      can_be_rehashed_(true) {
  InitializeCodeAddressMap();
}

ContextSerializer::~ContextSerializer() {
  OutputStatistics("ContextSerializer");
}

void ContextSerializer::Serialize(Context* o,
                                  const DisallowGarbageCollection& no_gc) {
  context_ = *o;
  DCHECK(context_.IsNativeContext());

  // Upon deserialization, references to the global proxy and its map will be
  // replaced.
  reference_map()->AddAttachedReference(context_.global_proxy());
  reference_map()->AddAttachedReference(context_.global_proxy().map());

  // The bootstrap snapshot has a code-stub context. When serializing the
  // context snapshot, it is chained into the weak context list on the isolate
  // and it's next context pointer may point to the code-stub context.  Clear
  // it before serializing, it will get re-added to the context list
  // explicitly when it's loaded.
  // TODO(v8:10416): These mutations should not observably affect the running
  // context.
  context_.set(Context::NEXT_CONTEXT_LINK,
               ReadOnlyRoots(isolate()).undefined_value());
  DCHECK(!context_.global_object().IsUndefined());
  // Reset math random cache to get fresh random numbers.
  MathRandom::ResetContext(context_);

  SanitizeNativeContextScope sanitize_native_context(
      isolate(), context_.native_context(), allow_active_isolate_for_testing(),
      no_gc);

  VisitRootPointer(Root::kStartupObjectCache, nullptr, FullObjectSlot(o));
  SerializeDeferredObjects();

  // Add section for embedder-serialized embedder fields.
  if (!embedder_fields_sink_.data()->empty()) {
    sink_.Put(kEmbedderFieldsData, "embedder fields data");
    sink_.Append(embedder_fields_sink_);
    sink_.Put(kSynchronize, "Finished with embedder fields data");
  }

  Pad();
}

void ContextSerializer::SerializeObjectImpl(Handle<HeapObject> obj,
                                            SlotType slot_type) {
  DCHECK(!ObjectIsBytecodeHandler(*obj));  // Only referenced in dispatch table.

  if (!allow_active_isolate_for_testing()) {
    // When serializing a snapshot intended for real use, we should not end up
    // at another native context.
    // But in test scenarios there is no way to avoid this. Since we only
    // serialize a single context in these cases, and this context does not
    // have to be executable, we can simply ignore this.
    DCHECK_IMPLIES(obj->IsNativeContext(), *obj == context_);
  }

  {
    DisallowGarbageCollection no_gc;
    HeapObject raw = *obj;
    if (SerializeHotObject(raw)) return;
    if (SerializeRoot(raw)) return;
    if (SerializeBackReference(raw)) return;
  }

  if (startup_serializer_->SerializeUsingReadOnlyObjectCache(&sink_, obj)) {
    return;
  }

  if (startup_serializer_->SerializeUsingSharedHeapObjectCache(&sink_, obj)) {
    return;
  }

  if (ShouldBeInTheStartupObjectCache(*obj)) {
    startup_serializer_->SerializeUsingStartupObjectCache(&sink_, obj);
    return;
  }

  // Pointers from the context snapshot to the objects in the startup snapshot
  // should go through the root array or through the startup object cache.
  // If this is not the case you may have to add something to the root array.
  DCHECK(!startup_serializer_->ReferenceMapContains(obj));
  // All the internalized strings that the context snapshot needs should be
  // either in the root table or in the shared heap object cache.
  DCHECK(!obj->IsInternalizedString());
  // Function and object templates are not context specific.
  DCHECK(!obj->IsTemplateInfo());

  InstanceType instance_type = obj->map().instance_type();
  if (InstanceTypeChecker::IsFeedbackVector(instance_type)) {
    // Clear literal boilerplates and feedback.
    Handle<FeedbackVector>::cast(obj)->ClearSlots(isolate());
  } else if (InstanceTypeChecker::IsJSObject(instance_type)) {
    if (SerializeJSObjectWithEmbedderFields(Handle<JSObject>::cast(obj))) {
      return;
    }
    if (InstanceTypeChecker::IsJSFunction(instance_type)) {
      DisallowGarbageCollection no_gc;
      // Unconditionally reset the JSFunction to its SFI's code, since we can't
      // serialize optimized code anyway.
      JSFunction closure = JSFunction::cast(*obj);
      if (closure.shared().HasBytecodeArray()) {
        closure.SetInterruptBudget(isolate());
      }
      closure.ResetIfCodeFlushed();
      if (closure.is_compiled()) {
        if (closure.shared().HasBaselineCode()) {
          closure.shared().FlushBaselineCode();
        }
        closure.set_code(closure.shared().GetCode(isolate()), kReleaseStore);
      }
    }
  }

  CheckRehashability(*obj);

  // Object has not yet been serialized.  Serialize it here.
  ObjectSerializer serializer(this, obj, &sink_);
  serializer.Serialize(slot_type);
}

bool ContextSerializer::ShouldBeInTheStartupObjectCache(HeapObject o) {
  // We can't allow scripts to be part of the context snapshot because they
  // contain a unique ID, and deserializing several context snapshots containing
  // script would cause dupes.
  return o.IsName() || o.IsScript() || o.IsSharedFunctionInfo() ||
         o.IsHeapNumber() || o.IsCode() || o.IsInstructionStream() ||
         o.IsScopeInfo() || o.IsAccessorInfo() || o.IsTemplateInfo() ||
         o.IsClassPositions() ||
         o.map() == ReadOnlyRoots(isolate()).fixed_cow_array_map();
}

bool ContextSerializer::ShouldBeInTheSharedObjectCache(HeapObject o) {
  // v8_flags.shared_string_table may be true during deserialization, so put
  // internalized strings into the shared object snapshot.
  return o.IsInternalizedString();
}

namespace {
bool DataIsEmpty(const StartupData& data) { return data.raw_size == 0; }
}  // anonymous namespace

bool ContextSerializer::SerializeJSObjectWithEmbedderFields(
    Handle<JSObject> obj) {
  DisallowGarbageCollection no_gc;
  JSObject js_obj = *obj;
  int embedder_fields_count = js_obj.GetEmbedderFieldCount();
  if (embedder_fields_count == 0) return false;
  CHECK_GT(embedder_fields_count, 0);
  DCHECK(!js_obj.NeedsRehashing(cage_base()));

  DisallowJavascriptExecution no_js(isolate());
  DisallowCompilation no_compile(isolate());

  v8::Local<v8::Object> api_obj = v8::Utils::ToLocal(obj);

  std::vector<EmbedderDataSlot::RawData> original_embedder_values;
  std::vector<StartupData> serialized_data;

  // 1) Iterate embedder fields. Hold onto the original value of the fields.
  //    Ignore references to heap objects since these are to be handled by the
  //    serializer. For aligned pointers, call the serialize callback. Hold
  //    onto the result.
  for (int i = 0; i < embedder_fields_count; i++) {
    EmbedderDataSlot embedder_data_slot(js_obj, i);
    original_embedder_values.emplace_back(
        embedder_data_slot.load_raw(isolate(), no_gc));
    Object object = embedder_data_slot.load_tagged();
    if (object.IsHeapObject()) {
      DCHECK(IsValidHeapObject(isolate()->heap(), HeapObject::cast(object)));
      serialized_data.push_back({nullptr, 0});
    } else {
      // If no serializer is provided and the field was empty, we serialize it
      // by default to nullptr.
      if (serialize_embedder_fields_.callback == nullptr &&
          object == Smi::zero()) {
        serialized_data.push_back({nullptr, 0});
      } else {
        DCHECK_NOT_NULL(serialize_embedder_fields_.callback);
        StartupData data = serialize_embedder_fields_.callback(
            api_obj, i, serialize_embedder_fields_.data);
        serialized_data.push_back(data);
      }
    }
  }

  // 2) Embedder fields for which the embedder callback produced non-zero
  //    serialized data should be considered aligned pointers to objects owned
  //    by the embedder. Clear these memory addresses to avoid non-determism
  //    in the snapshot. This is done separately to step 1 to no not interleave
  //    with embedder callbacks.
  for (int i = 0; i < embedder_fields_count; i++) {
    if (!DataIsEmpty(serialized_data[i])) {
      EmbedderDataSlot(js_obj, i).store_raw(isolate(), kNullAddress, no_gc);
    }
  }

  // 3) Serialize the object. References from embedder fields to heap objects or
  //    smis are serialized regularly.
  {
    AllowGarbageCollection allow_gc;
    ObjectSerializer(this, obj, &sink_).Serialize(SlotType::kAnySlot);
    // Reload raw pointer.
    js_obj = *obj;
  }

  // 4) Obtain back reference for the serialized object.
  const SerializerReference* reference =
      reference_map()->LookupReference(js_obj);
  DCHECK_NOT_NULL(reference);
  DCHECK(reference->is_back_reference());

  // 5) Write data returned by the embedder callbacks into a separate sink,
  //    headed by the back reference. Restore the original embedder fields.
  for (int i = 0; i < embedder_fields_count; i++) {
    StartupData data = serialized_data[i];
    if (DataIsEmpty(data)) continue;
    // Restore original values from cleared fields.
    EmbedderDataSlot(js_obj, i).store_raw(isolate(),
                                          original_embedder_values[i], no_gc);
    embedder_fields_sink_.Put(kNewObject, "embedder field holder");
    embedder_fields_sink_.PutInt(reference->back_ref_index(), "BackRefIndex");
    embedder_fields_sink_.PutInt(i, "embedder field index");
    embedder_fields_sink_.PutInt(data.raw_size, "embedder fields data size");
    embedder_fields_sink_.PutRaw(reinterpret_cast<const byte*>(data.data),
                                 data.raw_size, "embedder fields data");
    delete[] data.data;
  }

  // 6) The content of the separate sink is appended eventually to the default
  //    sink. The ensures that during deserialization, we call the deserializer
  //    callback at the end, and can guarantee that the deserialized objects are
  //    in a consistent state. See ContextSerializer::Serialize.
  return true;
}

void ContextSerializer::CheckRehashability(HeapObject obj) {
  if (!can_be_rehashed_) return;
  if (!obj.NeedsRehashing(cage_base())) return;
  if (obj.CanBeRehashed(cage_base())) return;
  can_be_rehashed_ = false;
}

}  // namespace internal
}  // namespace v8
