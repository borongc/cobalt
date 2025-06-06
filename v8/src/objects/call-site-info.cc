// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/call-site-info.h"

#include "src/base/strings.h"
#include "src/objects/call-site-info-inl.h"
#include "src/objects/shared-function-info.h"
#include "src/strings/string-builder-inl.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/debug/debug-wasm-objects.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {

bool CallSiteInfo::IsPromiseAll() const {
  if (!IsAsync()) return false;
  JSFunction fun = JSFunction::cast(function());
  return fun == fun.native_context().promise_all();
}

bool CallSiteInfo::IsPromiseAllSettled() const {
  if (!IsAsync()) return false;
  JSFunction fun = JSFunction::cast(function());
  return fun == fun.native_context().promise_all_settled();
}

bool CallSiteInfo::IsPromiseAny() const {
  if (!IsAsync()) return false;
  JSFunction fun = JSFunction::cast(function());
  return fun == fun.native_context().promise_any();
}

bool CallSiteInfo::IsNative() const {
  if (auto script = GetScript()) {
    return script->type() == Script::Type::kNative;
  }
  return false;
}

bool CallSiteInfo::IsEval() const {
  if (auto script = GetScript()) {
    return script->compilation_type() == Script::CompilationType::kEval;
  }
  return false;
}

bool CallSiteInfo::IsUserJavaScript() const {
#if V8_ENABLE_WEBASSEMBLY
  if (IsWasm()) return false;
#endif  // V8_ENABLE_WEBASSEMBLY
  return GetSharedFunctionInfo().IsUserJavaScript();
}

bool CallSiteInfo::IsMethodCall() const {
#if V8_ENABLE_WEBASSEMBLY
  if (IsWasm()) return false;
#endif  // V8_ENABLE_WEBASSEMBLY
  return !IsToplevel() && !IsConstructor();
}

bool CallSiteInfo::IsToplevel() const {
  return receiver_or_instance().IsJSGlobalProxy() ||
         receiver_or_instance().IsNullOrUndefined();
}

// static
int CallSiteInfo::GetLineNumber(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm() && !info->IsAsmJsWasm()) {
    return 1;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<Script> script;
  if (GetScript(isolate, info).ToHandle(&script)) {
    int position = GetSourcePosition(info);
    int line_number = Script::GetLineNumber(script, position) + 1;
    if (script->HasSourceURLComment()) {
      line_number -= script->line_offset();
    }
    return line_number;
  }
  return Message::kNoLineNumberInfo;
}

// static
int CallSiteInfo::GetColumnNumber(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
  int position = GetSourcePosition(info);
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm() && !info->IsAsmJsWasm()) {
    return position + 1;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<Script> script;
  if (GetScript(isolate, info).ToHandle(&script)) {
    Script::PositionInfo info;
    Script::GetPositionInfo(script, position, &info);
    int column_number = info.column + 1;
    if (script->HasSourceURLComment() && info.line == script->line_offset()) {
      column_number -= script->column_offset();
    }
    return column_number;
  }
  return Message::kNoColumnInfo;
}

// static
int CallSiteInfo::GetEnclosingLineNumber(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm() && !info->IsAsmJsWasm()) {
    return 1;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<Script> script;
  if (!GetScript(isolate, info).ToHandle(&script)) {
    return Message::kNoLineNumberInfo;
  }
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsAsmJsWasm()) {
    auto module = info->GetWasmInstance().module();
    auto func_index = info->GetWasmFunctionIndex();
    int position = wasm::GetSourcePosition(module, func_index, 0,
                                           info->IsAsmJsAtNumberConversion());
    return Script::GetLineNumber(script, position) + 1;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  int position = info->GetSharedFunctionInfo().function_token_position();
  return Script::GetLineNumber(script, position) + 1;
}

// static
int CallSiteInfo::GetEnclosingColumnNumber(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm() && !info->IsAsmJsWasm()) {
    auto module = info->GetWasmInstance().module();
    auto func_index = info->GetWasmFunctionIndex();
    return GetWasmFunctionOffset(module, func_index);
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<Script> script;
  if (!GetScript(isolate, info).ToHandle(&script)) {
    return Message::kNoColumnInfo;
  }
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsAsmJsWasm()) {
    auto module = info->GetWasmInstance().module();
    auto func_index = info->GetWasmFunctionIndex();
    int position = wasm::GetSourcePosition(module, func_index, 0,
                                           info->IsAsmJsAtNumberConversion());
    return Script::GetColumnNumber(script, position) + 1;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  int position = info->GetSharedFunctionInfo().function_token_position();
  return Script::GetColumnNumber(script, position) + 1;
}

int CallSiteInfo::GetScriptId() const {
  if (auto script = GetScript()) {
    return script->id();
  }
  return Message::kNoScriptIdInfo;
}

Object CallSiteInfo::GetScriptName() const {
  if (auto script = GetScript()) {
    return script->name();
  }
  return ReadOnlyRoots(GetIsolate()).null_value();
}

Object CallSiteInfo::GetScriptNameOrSourceURL() const {
  if (auto script = GetScript()) {
    return script->GetNameOrSourceURL();
  }
  return ReadOnlyRoots(GetIsolate()).null_value();
}

Object CallSiteInfo::GetScriptSource() const {
  if (auto script = GetScript()) {
    if (script->HasValidSource()) {
      return script->source();
    }
  }
  return ReadOnlyRoots(GetIsolate()).null_value();
}

Object CallSiteInfo::GetScriptSourceMappingURL() const {
  if (auto script = GetScript()) {
    return script->source_mapping_url();
  }
  return ReadOnlyRoots(GetIsolate()).null_value();
}

// static
Handle<String> CallSiteInfo::GetScriptHash(Handle<CallSiteInfo> info) {
  Handle<Script> script;
  Isolate* isolate = info->GetIsolate();
  if (!GetScript(isolate, info).ToHandle(&script)) {
    return isolate->factory()->empty_string();
  }
  if (script->HasValidSource()) {
    return Script::GetScriptHash(isolate, script, /*forceForInspector:*/ false);
  }
  return isolate->factory()->empty_string();
}

namespace {

MaybeHandle<String> FormatEvalOrigin(Isolate* isolate, Handle<Script> script) {
  Handle<Object> sourceURL(script->GetNameOrSourceURL(), isolate);
  if (sourceURL->IsString()) return Handle<String>::cast(sourceURL);

  IncrementalStringBuilder builder(isolate);
  builder.AppendCStringLiteral("eval at ");
  if (script->has_eval_from_shared()) {
    Handle<SharedFunctionInfo> eval_shared(script->eval_from_shared(), isolate);
    auto eval_name = SharedFunctionInfo::DebugName(isolate, eval_shared);
    if (eval_name->length() != 0) {
      builder.AppendString(eval_name);
    } else {
      builder.AppendCStringLiteral("<anonymous>");
    }
    if (eval_shared->script().IsScript()) {
      Handle<Script> eval_script(Script::cast(eval_shared->script()), isolate);
      builder.AppendCStringLiteral(" (");
      if (eval_script->compilation_type() == Script::CompilationType::kEval) {
        // Eval script originated from another eval.
        Handle<String> str;
        ASSIGN_RETURN_ON_EXCEPTION(
            isolate, str, FormatEvalOrigin(isolate, eval_script), String);
        builder.AppendString(str);
      } else {
        // eval script originated from "real" source.
        Handle<Object> eval_script_name(eval_script->name(), isolate);
        if (eval_script_name->IsString()) {
          builder.AppendString(Handle<String>::cast(eval_script_name));
          Script::PositionInfo info;
          if (Script::GetPositionInfo(eval_script,
                                      Script::GetEvalPosition(isolate, script),
                                      &info, Script::OffsetFlag::kNoOffset)) {
            builder.AppendCharacter(':');
            builder.AppendInt(info.line + 1);
            builder.AppendCharacter(':');
            builder.AppendInt(info.column + 1);
          }
        } else {
          builder.AppendCStringLiteral("unknown source");
        }
      }
      builder.AppendCharacter(')');
    }
  } else {
    builder.AppendCStringLiteral("<anonymous>");
  }
  return builder.Finish().ToHandleChecked();
}

}  // namespace

// static
Handle<PrimitiveHeapObject> CallSiteInfo::GetEvalOrigin(
    Handle<CallSiteInfo> info) {
  auto isolate = info->GetIsolate();
  Handle<Script> script;
  if (!GetScript(isolate, info).ToHandle(&script) ||
      script->compilation_type() != Script::CompilationType::kEval) {
    return isolate->factory()->undefined_value();
  }
  return FormatEvalOrigin(isolate, script).ToHandleChecked();
}

// static
Handle<PrimitiveHeapObject> CallSiteInfo::GetFunctionName(
    Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm()) {
    Handle<WasmModuleObject> module_object(
        info->GetWasmInstance().module_object(), isolate);
    uint32_t func_index = info->GetWasmFunctionIndex();
    Handle<String> name;
    if (WasmModuleObject::GetFunctionNameOrNull(isolate, module_object,
                                                func_index)
            .ToHandle(&name)) {
      return name;
    }
    return isolate->factory()->null_value();
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<JSFunction> function(JSFunction::cast(info->function()), isolate);
  Handle<String> name = JSFunction::GetDebugName(function);
  if (name->length() != 0) return name;
  if (info->IsEval()) return isolate->factory()->eval_string();
  return isolate->factory()->null_value();
}

// static
Handle<String> CallSiteInfo::GetFunctionDebugName(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm()) {
    return GetWasmFunctionDebugName(isolate,
                                    handle(info->GetWasmInstance(), isolate),
                                    info->GetWasmFunctionIndex());
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<JSFunction> function(JSFunction::cast(info->function()), isolate);
  Handle<String> name = JSFunction::GetDebugName(function);
  if (name->length() == 0 && info->IsEval()) {
    name = isolate->factory()->eval_string();
  }
  return name;
}

namespace {

PrimitiveHeapObject InferMethodNameFromFastObject(Isolate* isolate,
                                                  JSObject receiver,
                                                  JSFunction fun,
                                                  PrimitiveHeapObject name) {
  ReadOnlyRoots roots(isolate);
  Map map = receiver.map();
  DescriptorArray descriptors = map.instance_descriptors(isolate);
  for (auto i : map.IterateOwnDescriptors()) {
    PrimitiveHeapObject key = descriptors.GetKey(i);
    if (key.IsSymbol()) continue;
    auto details = descriptors.GetDetails(i);
    if (details.IsDontEnum()) continue;
    Object value;
    if (details.location() == PropertyLocation::kField) {
      auto field_index = FieldIndex::ForPropertyIndex(
          map, details.field_index(), details.representation());
      if (field_index.is_double()) continue;
      value = receiver.RawFastPropertyAt(isolate, field_index);
    } else {
      value = descriptors.GetStrongValue(i);
    }
    if (value != fun) {
      if (!value.IsAccessorPair()) continue;
      auto pair = AccessorPair::cast(value);
      if (pair.getter() != fun && pair.setter() != fun) continue;
    }
    if (name != key) {
      name = name.IsUndefined(isolate) ? key : roots.null_value();
    }
  }
  return name;
}

template <typename Dictionary>
PrimitiveHeapObject InferMethodNameFromDictionary(Isolate* isolate,
                                                  Dictionary dictionary,
                                                  JSFunction fun,
                                                  PrimitiveHeapObject name) {
  ReadOnlyRoots roots(isolate);
  for (auto i : dictionary.IterateEntries()) {
    Object key;
    if (!dictionary.ToKey(roots, i, &key)) continue;
    if (key.IsSymbol()) continue;
    auto details = dictionary.DetailsAt(i);
    if (details.IsDontEnum()) continue;
    auto value = dictionary.ValueAt(i);
    if (value != fun) {
      if (!value.IsAccessorPair()) continue;
      auto pair = AccessorPair::cast(value);
      if (pair.getter() != fun && pair.setter() != fun) continue;
    }
    if (name != key) {
      name = name.IsUndefined(isolate) ? PrimitiveHeapObject::cast(key)
                                       : roots.null_value();
    }
  }
  return name;
}

PrimitiveHeapObject InferMethodName(Isolate* isolate, JSReceiver receiver,
                                    JSFunction fun) {
  DisallowGarbageCollection no_gc;
  ReadOnlyRoots roots(isolate);
  PrimitiveHeapObject name = roots.undefined_value();
  for (PrototypeIterator it(isolate, receiver, kStartAtReceiver); !it.IsAtEnd();
       it.Advance()) {
    auto current = it.GetCurrent();
    if (!current.IsJSObject()) break;
    auto object = JSObject::cast(current);
    if (object.IsAccessCheckNeeded()) break;
    if (object.HasFastProperties()) {
      name = InferMethodNameFromFastObject(isolate, object, fun, name);
    } else if (object.IsJSGlobalObject()) {
      name = InferMethodNameFromDictionary(
          isolate, JSGlobalObject::cast(object).global_dictionary(kAcquireLoad),
          fun, name);
    } else if (V8_ENABLE_SWISS_NAME_DICTIONARY_BOOL) {
      name = InferMethodNameFromDictionary(
          isolate, object.property_dictionary_swiss(), fun, name);
    } else {
      name = InferMethodNameFromDictionary(
          isolate, object.property_dictionary(), fun, name);
    }
  }
  if (name.IsUndefined(isolate)) return roots.null_value();
  return name;
}

}  // namespace

// static
Handle<Object> CallSiteInfo::GetMethodName(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
  Handle<Object> receiver_or_instance(info->receiver_or_instance(), isolate);
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm()) return isolate->factory()->null_value();
#endif  // V8_ENABLE_WEBASSEMBLY
  if (receiver_or_instance->IsNullOrUndefined(isolate)) {
    return isolate->factory()->null_value();
  }

  Handle<JSReceiver> receiver =
      JSReceiver::ToObject(isolate, receiver_or_instance).ToHandleChecked();
  Handle<JSFunction> function =
      handle(JSFunction::cast(info->function()), isolate);
  Handle<String> name(function->shared().Name(), isolate);
  name = String::Flatten(isolate, name);

  // The static initializer function is not a method, so don't add a
  // class name, just return the function name.
  if (name->HasOneBytePrefix(base::CStrVector("<static_fields_initializer>"))) {
    return name;
  }

  // ES2015 gives getters and setters name prefixes which must
  // be stripped to find the property name.
  if (name->HasOneBytePrefix(base::CStrVector("get ")) ||
      name->HasOneBytePrefix(base::CStrVector("set "))) {
    name = isolate->factory()->NewProperSubString(name, 4, name->length());
  } else if (name->length() == 0) {
    // The function doesn't have a meaningful "name" property, however
    // the parser does store an inferred name "o.foo" for the common
    // case of `o.foo = function() {...}`, so see if we can derive a
    // property name to guess from that.
    name = handle(function->shared().inferred_name(), isolate);
    for (int index = name->length(); --index >= 0;) {
      if (name->Get(index, isolate) == '.') {
        name = isolate->factory()->NewProperSubString(name, index + 1,
                                                      name->length());
        break;
      }
    }
  }

  if (name->length() != 0) {
    PropertyKey key(isolate, Handle<Name>::cast(name));
    LookupIterator it(isolate, receiver, key,
                      LookupIterator::PROTOTYPE_CHAIN_SKIP_INTERCEPTOR);
    if (it.state() == LookupIterator::DATA) {
      if (it.GetDataValue().is_identical_to(function)) {
        return name;
      }
    } else if (it.state() == LookupIterator::ACCESSOR) {
      Handle<Object> accessors = it.GetAccessors();
      if (accessors->IsAccessorPair()) {
        Handle<AccessorPair> pair = Handle<AccessorPair>::cast(accessors);
        if (pair->getter() == *function || pair->setter() == *function) {
          return name;
        }
      }
    }
  }

  return handle(InferMethodName(isolate, *receiver, *function), isolate);
}

// static
Handle<Object> CallSiteInfo::GetTypeName(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
  if (!info->IsMethodCall()) {
    return isolate->factory()->null_value();
  }
  Handle<JSReceiver> receiver =
      JSReceiver::ToObject(isolate,
                           handle(info->receiver_or_instance(), isolate))
          .ToHandleChecked();
  if (receiver->IsJSProxy()) {
    return isolate->factory()->Proxy_string();
  }
  return JSReceiver::GetConstructorName(isolate, receiver);
}

#if V8_ENABLE_WEBASSEMBLY
uint32_t CallSiteInfo::GetWasmFunctionIndex() const {
  DCHECK(IsWasm());
  return Smi::ToInt(Smi::cast(function()));
}

WasmInstanceObject CallSiteInfo::GetWasmInstance() const {
  DCHECK(IsWasm());
  return WasmInstanceObject::cast(receiver_or_instance());
}

// static
Handle<Object> CallSiteInfo::GetWasmModuleName(Handle<CallSiteInfo> info) {
  Isolate* isolate = info->GetIsolate();
  if (info->IsWasm()) {
    Handle<String> name;
    auto module_object =
        handle(info->GetWasmInstance().module_object(), isolate);
    if (WasmModuleObject::GetModuleNameOrNull(isolate, module_object)
            .ToHandle(&name)) {
      return name;
    }
  }
  return isolate->factory()->null_value();
}
#endif  // V8_ENABLE_WEBASSEMBLY

// static
int CallSiteInfo::GetSourcePosition(Handle<CallSiteInfo> info) {
  if (info->flags() & kIsSourcePositionComputed) {
    return info->code_offset_or_source_position();
  }
  DCHECK(!info->IsPromiseAll());
  DCHECK(!info->IsPromiseAllSettled());
  DCHECK(!info->IsPromiseAny());
  int source_position =
      ComputeSourcePosition(info, info->code_offset_or_source_position());
  info->set_code_offset_or_source_position(source_position);
  info->set_flags(info->flags() | kIsSourcePositionComputed);
  return source_position;
}

// static
bool CallSiteInfo::ComputeLocation(Handle<CallSiteInfo> info,
                                   MessageLocation* location) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm()) {
    int pos = GetSourcePosition(info);
    Handle<Script> script(info->GetWasmInstance().module_object().script(),
                          isolate);
    *location = MessageLocation(script, pos, pos + 1);
    return true;
  }
#endif  // V8_ENABLE_WEBASSEMBLY

  Handle<SharedFunctionInfo> shared(info->GetSharedFunctionInfo(), isolate);
  if (!shared->IsSubjectToDebugging()) return false;
  Handle<Script> script(Script::cast(shared->script()), isolate);
  if (script->source().IsUndefined()) return false;
  if (info->flags() & kIsSourcePositionComputed ||
      (shared->HasBytecodeArray() &&
       shared->GetBytecodeArray(isolate).HasSourcePositionTable())) {
    int pos = GetSourcePosition(info);
    *location = MessageLocation(script, pos, pos + 1, shared);
  } else {
    int code_offset = info->code_offset_or_source_position();
    *location = MessageLocation(script, shared, code_offset);
  }
  return true;
}

// static
int CallSiteInfo::ComputeSourcePosition(Handle<CallSiteInfo> info, int offset) {
  Isolate* isolate = info->GetIsolate();
#if V8_ENABLE_WEBASSEMBLY
  if (info->IsWasm()) {
    auto module = info->GetWasmInstance().module();
    uint32_t func_index = info->GetWasmFunctionIndex();
    return wasm::GetSourcePosition(module, func_index, offset,
                                   info->IsAsmJsAtNumberConversion());
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Handle<SharedFunctionInfo> shared(info->GetSharedFunctionInfo(), isolate);
  SharedFunctionInfo::EnsureSourcePositionsAvailable(isolate, shared);
  return AbstractCode::cast(info->code_object())
      .SourcePosition(isolate, offset);
}

base::Optional<Script> CallSiteInfo::GetScript() const {
#if V8_ENABLE_WEBASSEMBLY
  if (IsWasm()) {
    return GetWasmInstance().module_object().script();
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  Object script = GetSharedFunctionInfo().script();
  if (script.IsScript()) return Script::cast(script);
  return base::nullopt;
}

SharedFunctionInfo CallSiteInfo::GetSharedFunctionInfo() const {
#if V8_ENABLE_WEBASSEMBLY
  DCHECK(!IsWasm());
#endif  // V8_ENABLE_WEBASSEMBLY
  return JSFunction::cast(function()).shared();
}

// static
MaybeHandle<Script> CallSiteInfo::GetScript(Isolate* isolate,
                                            Handle<CallSiteInfo> info) {
  if (auto script = info->GetScript()) {
    return handle(*script, isolate);
  }
  return kNullMaybeHandle;
}

namespace {

bool IsNonEmptyString(Handle<Object> object) {
  return (object->IsString() && String::cast(*object).length() > 0);
}

void AppendFileLocation(Isolate* isolate, Handle<CallSiteInfo> frame,
                        IncrementalStringBuilder* builder) {
  Handle<Object> script_name_or_source_url(frame->GetScriptNameOrSourceURL(),
                                           isolate);
  if (!script_name_or_source_url->IsString() && frame->IsEval()) {
    builder->AppendString(
        Handle<String>::cast(CallSiteInfo::GetEvalOrigin(frame)));
    // Expecting source position to follow.
    builder->AppendCStringLiteral(", ");
  }

  if (IsNonEmptyString(script_name_or_source_url)) {
    builder->AppendString(Handle<String>::cast(script_name_or_source_url));
  } else {
    // Source code does not originate from a file and is not native, but we
    // can still get the source position inside the source string, e.g. in
    // an eval string.
    builder->AppendCStringLiteral("<anonymous>");
  }

  int line_number = CallSiteInfo::GetLineNumber(frame);
  if (line_number != Message::kNoLineNumberInfo) {
    builder->AppendCharacter(':');
    builder->AppendInt(line_number);

    int column_number = CallSiteInfo::GetColumnNumber(frame);
    if (column_number != Message::kNoColumnInfo) {
      builder->AppendCharacter(':');
      builder->AppendInt(column_number);
    }
  }
}

// Returns true iff
// 1. the subject ends with '.' + pattern, or
// 2. subject == pattern.
bool StringEndsWithMethodName(Isolate* isolate, Handle<String> subject,
                              Handle<String> pattern) {
  if (String::Equals(isolate, subject, pattern)) return true;

  FlatStringReader subject_reader(isolate, String::Flatten(isolate, subject));
  FlatStringReader pattern_reader(isolate, String::Flatten(isolate, pattern));

  int pattern_index = pattern_reader.length() - 1;
  int subject_index = subject_reader.length() - 1;
  for (int i = 0; i <= pattern_reader.length(); i++) {  // Iterate over len + 1.
    if (subject_index < 0) {
      return false;
    }

    const base::uc32 subject_char = subject_reader.Get(subject_index);
    if (i == pattern_reader.length()) {
      if (subject_char != '.') return false;
    } else if (subject_char != pattern_reader.Get(pattern_index)) {
      return false;
    }

    pattern_index--;
    subject_index--;
  }

  return true;
}

void AppendMethodCall(Isolate* isolate, Handle<CallSiteInfo> frame,
                      IncrementalStringBuilder* builder) {
  Handle<Object> type_name = CallSiteInfo::GetTypeName(frame);
  Handle<Object> method_name = CallSiteInfo::GetMethodName(frame);
  Handle<Object> function_name = CallSiteInfo::GetFunctionName(frame);

  Handle<Object> receiver(frame->receiver_or_instance(), isolate);
  if (receiver->IsJSClassConstructor()) {
    Handle<JSFunction> function = Handle<JSFunction>::cast(receiver);
    Handle<String> class_name = JSFunction::GetDebugName(function);
    if (class_name->length() != 0) {
      type_name = class_name;
    }
  }
  if (IsNonEmptyString(function_name)) {
    Handle<String> function_string = Handle<String>::cast(function_name);
    if (IsNonEmptyString(type_name)) {
      Handle<String> type_string = Handle<String>::cast(type_name);
      if (String::IsIdentifier(isolate, function_string) &&
          !String::Equals(isolate, function_string, type_string)) {
        builder->AppendString(type_string);
        builder->AppendCharacter('.');
      }
    }
    builder->AppendString(function_string);

    if (IsNonEmptyString(method_name)) {
      Handle<String> method_string = Handle<String>::cast(method_name);
      if (!StringEndsWithMethodName(isolate, function_string, method_string)) {
        builder->AppendCStringLiteral(" [as ");
        builder->AppendString(method_string);
        builder->AppendCharacter(']');
      }
    }
  } else {
    if (IsNonEmptyString(type_name)) {
      builder->AppendString(Handle<String>::cast(type_name));
      builder->AppendCharacter('.');
    }
    if (IsNonEmptyString(method_name)) {
      builder->AppendString(Handle<String>::cast(method_name));
    } else {
      builder->AppendCStringLiteral("<anonymous>");
    }
  }
}

void SerializeJSStackFrame(Isolate* isolate, Handle<CallSiteInfo> frame,
                           IncrementalStringBuilder* builder) {
  Handle<Object> function_name = CallSiteInfo::GetFunctionName(frame);
  if (frame->IsAsync()) {
    builder->AppendCStringLiteral("async ");
    if (frame->IsPromiseAll() || frame->IsPromiseAny() ||
        frame->IsPromiseAllSettled()) {
      builder->AppendCStringLiteral("Promise.");
      builder->AppendString(Handle<String>::cast(function_name));
      builder->AppendCStringLiteral(" (index ");
      builder->AppendInt(CallSiteInfo::GetSourcePosition(frame));
      builder->AppendCharacter(')');
      return;
    }
  }
  if (frame->IsMethodCall()) {
    AppendMethodCall(isolate, frame, builder);
  } else if (frame->IsConstructor()) {
    builder->AppendCStringLiteral("new ");
    if (IsNonEmptyString(function_name)) {
      builder->AppendString(Handle<String>::cast(function_name));
    } else {
      builder->AppendCStringLiteral("<anonymous>");
    }
  } else if (IsNonEmptyString(function_name)) {
    builder->AppendString(Handle<String>::cast(function_name));
  } else {
    AppendFileLocation(isolate, frame, builder);
    return;
  }
  builder->AppendCStringLiteral(" (");
  AppendFileLocation(isolate, frame, builder);
  builder->AppendCharacter(')');
}

#if V8_ENABLE_WEBASSEMBLY
void SerializeWasmStackFrame(Isolate* isolate, Handle<CallSiteInfo> frame,
                             IncrementalStringBuilder* builder) {
  Handle<Object> module_name = CallSiteInfo::GetWasmModuleName(frame);
  Handle<Object> function_name = CallSiteInfo::GetFunctionName(frame);
  const bool has_name = !module_name->IsNull() || !function_name->IsNull();
  if (has_name) {
    if (module_name->IsNull()) {
      builder->AppendString(Handle<String>::cast(function_name));
    } else {
      builder->AppendString(Handle<String>::cast(module_name));
      if (!function_name->IsNull()) {
        builder->AppendCharacter('.');
        builder->AppendString(Handle<String>::cast(function_name));
      }
    }
    builder->AppendCStringLiteral(" (");
  }

  Handle<Object> url(frame->GetScriptNameOrSourceURL(), isolate);
  if (IsNonEmptyString(url)) {
    builder->AppendString(Handle<String>::cast(url));
  } else {
    builder->AppendCStringLiteral("<anonymous>");
  }
  builder->AppendCharacter(':');

  const int wasm_func_index = frame->GetWasmFunctionIndex();
  builder->AppendCStringLiteral("wasm-function[");
  builder->AppendInt(wasm_func_index);
  builder->AppendCStringLiteral("]:");

  char buffer[16];
  SNPrintF(base::ArrayVector(buffer), "0x%x",
           CallSiteInfo::GetColumnNumber(frame) - 1);
  builder->AppendCString(buffer);

  if (has_name) builder->AppendCharacter(')');
}
#endif  // V8_ENABLE_WEBASSEMBLY

}  // namespace

void SerializeCallSiteInfo(Isolate* isolate, Handle<CallSiteInfo> frame,
                           IncrementalStringBuilder* builder) {
#if V8_ENABLE_WEBASSEMBLY
  if (frame->IsWasm() && !frame->IsAsmJsWasm()) {
    SerializeWasmStackFrame(isolate, frame, builder);
    return;
  }
#endif  // V8_ENABLE_WEBASSEMBLY
  SerializeJSStackFrame(isolate, frame, builder);
}

MaybeHandle<String> SerializeCallSiteInfo(Isolate* isolate,
                                          Handle<CallSiteInfo> frame) {
  IncrementalStringBuilder builder(isolate);
  SerializeCallSiteInfo(isolate, frame, &builder);
  return builder.Finish();
}

}  // namespace internal
}  // namespace v8
