// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/snapshot.h"

#include "platform/assert.h"
#include "vm/bigint_operations.h"
#include "vm/bootstrap.h"
#include "vm/class_finalizer.h"
#include "vm/exceptions.h"
#include "vm/heap.h"
#include "vm/longjump.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/snapshot_ids.h"
#include "vm/symbols.h"

namespace dart {

static const int kNumInitialReferencesInFullSnapshot = 160 * KB;
static const int kNumInitialReferences = 64;


static bool IsSingletonClassId(intptr_t class_id) {
  // Check if this is a singleton object class which is shared by all isolates.
  return ((class_id >= kClassCid && class_id <= kUnwindErrorCid) ||
          (class_id >= kNullCid && class_id <= kVoidCid));
}


static bool IsObjectStoreClassId(intptr_t class_id) {
  // Check if this is a class which is stored in the object store.
  return (class_id == kObjectCid ||
          (class_id >= kInstanceCid && class_id <= kWeakPropertyCid) ||
          RawObject::IsStringClassId(class_id) ||
          RawObject::IsTypedDataClassId(class_id) ||
          RawObject::IsExternalTypedDataClassId(class_id));
}


static bool IsObjectStoreTypeId(intptr_t index) {
  // Check if this is a type which is stored in the object store.
  return (index >= kObjectType && index <= kArrayType);
}


static intptr_t ClassIdFromObjectId(intptr_t object_id) {
  ASSERT(object_id > kClassIdsOffset);
  intptr_t class_id = (object_id - kClassIdsOffset);
  return class_id;
}


static intptr_t ObjectIdFromClassId(intptr_t class_id) {
  ASSERT((class_id > kIllegalCid) && (class_id < kNumPredefinedCids));
  ASSERT(!RawObject::IsTypedDataViewClassId(class_id));
  return (class_id + kClassIdsOffset);
}


static RawType* GetType(ObjectStore* object_store, int index) {
  switch (index) {
    case kObjectType: return object_store->object_type();
    case kNullType: return object_store->null_type();
    case kDynamicType: return object_store->dynamic_type();
    case kVoidType: return object_store->void_type();
    case kFunctionType: return object_store->function_type();
    case kNumberType: return object_store->number_type();
    case kSmiType: return object_store->smi_type();
    case kMintType: return object_store->mint_type();
    case kDoubleType: return object_store->double_type();
    case kIntType: return object_store->int_type();
    case kBoolType: return object_store->bool_type();
    case kStringType: return object_store->string_type();
    case kArrayType: return object_store->array_type();
    default: break;
  }
  UNREACHABLE();
  return Type::null();
}


static int GetTypeIndex(ObjectStore* object_store, const RawType* raw_type) {
  ASSERT(raw_type->IsHeapObject());
  if (raw_type == object_store->object_type()) {
    return kObjectType;
  } else if (raw_type == object_store->null_type()) {
    return kNullType;
  } else if (raw_type == object_store->dynamic_type()) {
    return kDynamicType;
  } else if (raw_type == object_store->void_type()) {
    return kVoidType;
  } else if (raw_type == object_store->function_type()) {
    return kFunctionType;
  } else if (raw_type == object_store->number_type()) {
    return kNumberType;
  } else if (raw_type == object_store->smi_type()) {
    return kSmiType;
  } else if (raw_type == object_store->mint_type()) {
    return kMintType;
  } else if (raw_type == object_store->double_type()) {
    return kDoubleType;
  } else if (raw_type == object_store->int_type()) {
    return kIntType;
  } else if (raw_type == object_store->bool_type()) {
    return kBoolType;
  } else if (raw_type == object_store->string_type()) {
    return kStringType;
  } else if (raw_type == object_store->array_type()) {
    return kArrayType;
  }
  return kInvalidIndex;
}


// TODO(5411462): Temporary setup of snapshot for testing purposes,
// the actual creation of a snapshot maybe done differently.
const Snapshot* Snapshot::SetupFromBuffer(const void* raw_memory) {
  ASSERT(raw_memory != NULL);
  ASSERT(kHeaderSize == sizeof(Snapshot));
  ASSERT(kLengthIndex == length_offset());
  ASSERT((kSnapshotFlagIndex * sizeof(int32_t)) == kind_offset());
  ASSERT((kHeapObjectTag & kInlined));
  // No object can have kFreeBit and kMarkBit set simultaneously. If kFreeBit
  // is set then the rest of tags is a pointer to the next FreeListElement which
  // is kObjectAlignment aligned and has at least 2 lower bits set to zero.
  ASSERT(kObjectId ==
         ((1 << RawObject::kFreeBit) | (1 << RawObject::kMarkBit)));
  ASSERT((kObjectAlignmentMask & kObjectId) == kObjectId);
  const Snapshot* snapshot = reinterpret_cast<const Snapshot*>(raw_memory);
  return snapshot;
}


RawSmi* BaseReader::ReadAsSmi() {
  intptr_t value = ReadIntptrValue();
  ASSERT((value & kSmiTagMask) == kSmiTag);
  return reinterpret_cast<RawSmi*>(value);
}


intptr_t BaseReader::ReadSmiValue() {
  return  Smi::Value(ReadAsSmi());
}


SnapshotReader::SnapshotReader(const uint8_t* buffer,
                               intptr_t size,
                               Snapshot::Kind kind,
                               Isolate* isolate)
    : BaseReader(buffer, size),
      kind_(kind),
      isolate_(isolate),
      cls_(Class::Handle()),
      obj_(Object::Handle()),
      str_(String::Handle()),
      library_(Library::Handle()),
      type_(AbstractType::Handle()),
      type_arguments_(AbstractTypeArguments::Handle()),
      tokens_(Array::Handle()),
      stream_(TokenStream::Handle()),
      data_(ExternalTypedData::Handle()),
      error_(UnhandledException::Handle()),
      backward_references_((kind == Snapshot::kFull) ?
                           kNumInitialReferencesInFullSnapshot :
                           kNumInitialReferences) {
}


RawObject* SnapshotReader::ReadObject() {
  // Setup for long jump in case there is an exception while reading.
  LongJump* base = isolate()->long_jump_base();
  LongJump jump;
  isolate()->set_long_jump_base(&jump);
  const Instance& null_object = Instance::Handle();
  *ErrorHandle() = UnhandledException::New(null_object, null_object);
  if (setjmp(*jump.Set()) == 0) {
    Object& obj = Object::Handle(ReadObjectImpl());
    for (intptr_t i = 0; i < backward_references_.length(); i++) {
      if (!backward_references_[i]->is_deserialized()) {
        ReadObjectImpl();
        backward_references_[i]->set_state(kIsDeserialized);
      }
    }
    isolate()->set_long_jump_base(base);
    return obj.raw();
  } else {
    // An error occurred while reading, return the error object.
    const Error& err = Error::Handle(isolate()->object_store()->sticky_error());
    isolate()->object_store()->clear_sticky_error();
    isolate()->set_long_jump_base(base);
    return err.raw();
  }
}


RawClass* SnapshotReader::ReadClassId(intptr_t object_id) {
  ASSERT(kind_ != Snapshot::kFull);
  // Read the class header information and lookup the class.
  intptr_t class_header = ReadIntptrValue();
  ASSERT((class_header & kSmiTagMask) != kSmiTag);
  Class& cls = Class::ZoneHandle(isolate(), Class::null());
  cls = LookupInternalClass(class_header);
  AddBackRef(object_id, &cls, kIsDeserialized);
  if (cls.IsNull()) {
    // Read the library/class information and lookup the class.
    str_ ^= ReadObjectImpl(class_header);
    library_ = Library::LookupLibrary(str_);
    ASSERT(!library_.IsNull());
    str_ ^= ReadObjectImpl();
    cls = library_.LookupClass(str_);
  }
  ASSERT(!cls.IsNull());
  return cls.raw();
}


RawObject* SnapshotReader::ReadObjectImpl() {
  int64_t value = Read<int64_t>();
  if ((value & kSmiTagMask) == kSmiTag) {
    return NewInteger(value);
  }
  return ReadObjectImpl(value);
}


RawObject* SnapshotReader::ReadObjectImpl(intptr_t header_value) {
  ASSERT((header_value <= kIntptrMax) && (header_value >= kIntptrMin));
  if (IsVMIsolateObject(header_value)) {
    return ReadVMIsolateObject(header_value);
  } else {
    if (SerializedHeaderTag::decode(header_value) == kObjectId) {
      return ReadIndexedObject(SerializedHeaderData::decode(header_value));
    }
    ASSERT(SerializedHeaderTag::decode(header_value) == kInlined);
    return ReadInlinedObject(SerializedHeaderData::decode(header_value));
  }
}


RawObject* SnapshotReader::ReadObjectRef() {
  int64_t header_value = Read<int64_t>();
  if ((header_value & kSmiTagMask) == kSmiTag) {
    return NewInteger(header_value);
  }
  ASSERT((header_value <= kIntptrMax) && (header_value >= kIntptrMin));
  if (IsVMIsolateObject(header_value)) {
    return ReadVMIsolateObject(header_value);
  } else if (SerializedHeaderTag::decode(header_value) == kObjectId) {
    return ReadIndexedObject(SerializedHeaderData::decode(header_value));
  }
  ASSERT(SerializedHeaderTag::decode(header_value) == kInlined);
  intptr_t object_id = SerializedHeaderData::decode(header_value);
  ASSERT(GetBackRef(object_id) == NULL);

  // Read the class header information and lookup the class.
  intptr_t class_header = ReadIntptrValue();

  // Since we are only reading an object reference, If it is an instance kind
  // then we only need to figure out the class of the object and allocate an
  // instance of it. The individual fields will be read later.
  if (SerializedHeaderData::decode(class_header) == kInstanceObjectId) {
    Instance& result = Instance::ZoneHandle(isolate(), Instance::null());
    AddBackRef(object_id, &result, kIsNotDeserialized);

    cls_ ^= ReadObjectImpl();  // Read class information.
    ASSERT(!cls_.IsNull());
    intptr_t instance_size = cls_.instance_size();
    ASSERT(instance_size > 0);
    if (kind_ == Snapshot::kFull) {
      result ^= AllocateUninitialized(cls_, instance_size);
    } else {
      result ^= Object::Allocate(cls_.id(), instance_size, HEAP_SPACE(kind_));
    }
    return result.raw();
  }
  ASSERT((class_header & kSmiTagMask) != kSmiTag);
  cls_ = LookupInternalClass(class_header);
  ASSERT(!cls_.IsNull());

  // Similarly Array and ImmutableArray objects are also similarly only
  // allocated here, the individual array elements are read later.
  intptr_t class_id = cls_.id();
  if (class_id == kArrayCid) {
    // Read the length and allocate an object based on the len.
    intptr_t len = ReadSmiValue();
    Array& array = Array::ZoneHandle(
        isolate(),
        ((kind_ == Snapshot::kFull) ?
         NewArray(len) : Array::New(len, HEAP_SPACE(kind_))));
    AddBackRef(object_id, &array, kIsNotDeserialized);

    return array.raw();
  }
  if (class_id == kImmutableArrayCid) {
    // Read the length and allocate an object based on the len.
    intptr_t len = ReadSmiValue();
    ImmutableArray& array = ImmutableArray::ZoneHandle(
        isolate(),
        (kind_ == Snapshot::kFull) ?
        NewImmutableArray(len) : ImmutableArray::New(len, HEAP_SPACE(kind_)));
    AddBackRef(object_id, &array, kIsNotDeserialized);

    return array.raw();
  }

  // For all other internal VM classes we read the object inline.
  intptr_t tags = ReadIntptrValue();
  switch (class_id) {
#define SNAPSHOT_READ(clazz)                                                   \
    case clazz::kClassId: {                                                    \
      obj_ = clazz::ReadFrom(this, object_id, tags, kind_);                    \
      break;                                                                   \
    }
    CLASS_LIST_NO_OBJECT(SNAPSHOT_READ)
#undef SNAPSHOT_READ
#define SNAPSHOT_READ(clazz)                                                   \
    case kTypedData##clazz##Cid:                                               \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_READ) {
      obj_ = TypedData::ReadFrom(this, object_id, tags, kind_);
      break;
    }
#undef SNAPSHOT_READ
#define SNAPSHOT_READ(clazz)                                                   \
    case kExternalTypedData##clazz##Cid:                                       \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_READ) {
      obj_ = ExternalTypedData::ReadFrom(this, object_id, tags, kind_);
      break;
    }
#undef SNAPSHOT_READ
    default: UNREACHABLE(); break;
  }
  if (kind_ == Snapshot::kFull) {
    obj_.SetCreatedFromSnapshot();
  }
  return obj_.raw();
}


void SnapshotReader::AddBackRef(intptr_t id,
                                Object* obj,
                                DeserializeState state) {
  intptr_t index = (id - kMaxPredefinedObjectIds);
  ASSERT(index == backward_references_.length());
  BackRefNode* node = new BackRefNode(obj, state);
  ASSERT(node != NULL);
  backward_references_.Add(node);
}


Object* SnapshotReader::GetBackRef(intptr_t id) {
  ASSERT(id >= kMaxPredefinedObjectIds);
  intptr_t index = (id - kMaxPredefinedObjectIds);
  if (index < backward_references_.length()) {
    return backward_references_[index]->reference();
  }
  return NULL;
}


void SnapshotReader::ReadFullSnapshot() {
  ASSERT(kind_ == Snapshot::kFull);
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  ObjectStore* object_store = isolate->object_store();
  ASSERT(object_store != NULL);
  NoGCScope no_gc;

  // TODO(asiva): Add a check here to ensure we have the right heap
  // size for the full snapshot being read.

  // Read in all the objects stored in the object store.
  intptr_t num_flds = (object_store->to() - object_store->from());
  for (intptr_t i = 0; i <= num_flds; i++) {
    *(object_store->from() + i) = ReadObjectImpl();
  }
  for (intptr_t i = 0; i < backward_references_.length(); i++) {
    if (!backward_references_[i]->is_deserialized()) {
      ReadObjectImpl();
      backward_references_[i]->set_state(kIsDeserialized);
    }
  }

  // Setup native resolver for bootstrap impl.
  Bootstrap::SetupNativeResolver();
}


#define ALLOC_NEW_OBJECT_WITH_LEN(type, class_obj, length)                     \
  ASSERT(kind_ == Snapshot::kFull);                                            \
  ASSERT(isolate()->no_gc_scope_depth() != 0);                                 \
  cls_ = class_obj;                                                            \
  Raw##type* obj = reinterpret_cast<Raw##type*>(                               \
      AllocateUninitialized(cls_, type::InstanceSize(length)));                \
  obj->ptr()->length_ = Smi::New(length);                                      \
  return obj;                                                                  \


RawArray* SnapshotReader::NewArray(intptr_t len) {
  ALLOC_NEW_OBJECT_WITH_LEN(Array, object_store()->array_class(), len);
}


RawImmutableArray* SnapshotReader::NewImmutableArray(intptr_t len) {
  ALLOC_NEW_OBJECT_WITH_LEN(ImmutableArray,
                            object_store()->immutable_array_class(),
                            len);
}


RawOneByteString* SnapshotReader::NewOneByteString(intptr_t len) {
  ALLOC_NEW_OBJECT_WITH_LEN(OneByteString,
                            object_store()->one_byte_string_class(),
                            len);
}


RawTwoByteString* SnapshotReader::NewTwoByteString(intptr_t len) {
  ALLOC_NEW_OBJECT_WITH_LEN(TwoByteString,
                            object_store()->two_byte_string_class(),
                            len);
}


RawTypeArguments* SnapshotReader::NewTypeArguments(intptr_t len) {
  ALLOC_NEW_OBJECT_WITH_LEN(TypeArguments,
                            Object::type_arguments_class(),
                            len);
}


RawTokenStream* SnapshotReader::NewTokenStream(intptr_t len) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = Object::token_stream_class();
  stream_ = reinterpret_cast<RawTokenStream*>(
      AllocateUninitialized(cls_, TokenStream::InstanceSize()));
  cls_ = isolate()->class_table()->At(kExternalTypedDataUint8ArrayCid);
  uint8_t* array = const_cast<uint8_t*>(CurrentBufferAddress());
  ASSERT(array != NULL);
  Advance(len);
  data_ = reinterpret_cast<RawExternalTypedData*>(
      AllocateUninitialized(cls_, ExternalTypedData::InstanceSize()));
  data_.SetData(array);
  data_.SetLength(len);
  stream_.SetStream(data_);
  return stream_.raw();
}


RawContext* SnapshotReader::NewContext(intptr_t num_variables) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = Object::context_class();
  RawContext* obj = reinterpret_cast<RawContext*>(
      AllocateUninitialized(cls_, Context::InstanceSize(num_variables)));
  obj->ptr()->num_variables_ = num_variables;
  return obj;
}


RawClass* SnapshotReader::NewClass(intptr_t class_id) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  if (class_id < kNumPredefinedCids) {
    ASSERT((class_id >= kInstanceCid) &&
           (class_id <= kExternalTypedDataFloat32x4ArrayCid));
    return isolate()->class_table()->At(class_id);
  }
  cls_ = Object::class_class();
  RawClass* obj = reinterpret_cast<RawClass*>(
      AllocateUninitialized(cls_, Class::InstanceSize()));
  Instance fake;
  obj->ptr()->handle_vtable_ = fake.vtable();
  cls_ = obj;
  cls_.set_id(kIllegalCid);
  isolate()->class_table()->Register(cls_);
  return cls_.raw();
}


RawMint* SnapshotReader::NewMint(int64_t value) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = object_store()->mint_class();
  RawMint* obj = reinterpret_cast<RawMint*>(
      AllocateUninitialized(cls_, Mint::InstanceSize()));
  obj->ptr()->value_ = value;
  return obj;
}


RawBigint* SnapshotReader::NewBigint(const char* hex_string) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = object_store()->bigint_class();
  intptr_t bigint_length = BigintOperations::ComputeChunkLength(hex_string);
  RawBigint* obj = reinterpret_cast<RawBigint*>(
      AllocateUninitialized(cls_, Bigint::InstanceSize(bigint_length)));
  obj->ptr()->allocated_length_ = bigint_length;
  obj->ptr()->signed_length_ = bigint_length;
  BigintOperations::FromHexCString(hex_string, Bigint::Handle(obj));
  return obj;
}


RawDouble* SnapshotReader::NewDouble(double value) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = object_store()->double_class();
  RawDouble* obj = reinterpret_cast<RawDouble*>(
      AllocateUninitialized(cls_, Double::InstanceSize()));
  obj->ptr()->value_ = value;
  return obj;
}


#define ALLOC_NEW_OBJECT(type, class_obj)                                      \
  ASSERT(kind_ == Snapshot::kFull);                                            \
  ASSERT(isolate()->no_gc_scope_depth() != 0);                                 \
  cls_ = class_obj;                                                            \
  return reinterpret_cast<Raw##type*>(                                         \
      AllocateUninitialized(cls_, type::InstanceSize()));                      \


RawUnresolvedClass* SnapshotReader::NewUnresolvedClass() {
  ALLOC_NEW_OBJECT(UnresolvedClass, Object::unresolved_class_class());
}


RawType* SnapshotReader::NewType() {
  ALLOC_NEW_OBJECT(Type, object_store()->type_class());
}


RawTypeParameter* SnapshotReader::NewTypeParameter() {
  ALLOC_NEW_OBJECT(TypeParameter, object_store()->type_parameter_class());
}


RawBoundedType* SnapshotReader::NewBoundedType() {
  ALLOC_NEW_OBJECT(BoundedType, object_store()->bounded_type_class());
}


RawMixinAppType* SnapshotReader::NewMixinAppType() {
  ALLOC_NEW_OBJECT(MixinAppType, object_store()->mixin_app_type_class());
}


RawPatchClass* SnapshotReader::NewPatchClass() {
  ALLOC_NEW_OBJECT(PatchClass, Object::patch_class_class());
}


RawClosureData* SnapshotReader::NewClosureData() {
  ALLOC_NEW_OBJECT(ClosureData, Object::closure_data_class());
}


RawRedirectionData* SnapshotReader::NewRedirectionData() {
  ALLOC_NEW_OBJECT(RedirectionData, Object::redirection_data_class());
}


RawFunction* SnapshotReader::NewFunction() {
  ALLOC_NEW_OBJECT(Function, Object::function_class());
}


RawField* SnapshotReader::NewField() {
  ALLOC_NEW_OBJECT(Field, Object::field_class());
}


RawLibrary* SnapshotReader::NewLibrary() {
  ALLOC_NEW_OBJECT(Library, Object::library_class());
}


RawLibraryPrefix* SnapshotReader::NewLibraryPrefix() {
  ALLOC_NEW_OBJECT(LibraryPrefix, Object::library_prefix_class());
}


RawNamespace* SnapshotReader::NewNamespace() {
  ALLOC_NEW_OBJECT(Namespace, Object::namespace_class());
}


RawScript* SnapshotReader::NewScript() {
  ALLOC_NEW_OBJECT(Script, Object::script_class());
}


RawLiteralToken* SnapshotReader::NewLiteralToken() {
  ALLOC_NEW_OBJECT(LiteralToken, Object::literal_token_class());
}


RawGrowableObjectArray* SnapshotReader::NewGrowableObjectArray() {
  ALLOC_NEW_OBJECT(GrowableObjectArray,
                   object_store()->growable_object_array_class());
}


RawFloat32x4* SnapshotReader::NewFloat32x4(float v0, float v1, float v2,
                                           float v3) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = object_store()->float32x4_class();
  RawFloat32x4* obj = reinterpret_cast<RawFloat32x4*>(
      AllocateUninitialized(cls_, Float32x4::InstanceSize()));
  obj->ptr()->value_[0] = v0;
  obj->ptr()->value_[1] = v1;
  obj->ptr()->value_[2] = v2;
  obj->ptr()->value_[3] = v3;
  return obj;
}


RawUint32x4* SnapshotReader::NewUint32x4(uint32_t v0, uint32_t v1, uint32_t v2,
                                         uint32_t v3) {
  ASSERT(kind_ == Snapshot::kFull);
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  cls_ = object_store()->uint32x4_class();
  RawUint32x4* obj = reinterpret_cast<RawUint32x4*>(
      AllocateUninitialized(cls_, Uint32x4::InstanceSize()));
  obj->ptr()->value_[0] = v0;
  obj->ptr()->value_[1] = v1;
  obj->ptr()->value_[2] = v2;
  obj->ptr()->value_[3] = v3;
  return obj;
}


RawApiError* SnapshotReader::NewApiError() {
  ALLOC_NEW_OBJECT(ApiError, Object::api_error_class());
}


RawLanguageError* SnapshotReader::NewLanguageError() {
  ALLOC_NEW_OBJECT(LanguageError, Object::language_error_class());
}


RawObject* SnapshotReader::NewInteger(int64_t value) {
  ASSERT((value & kSmiTagMask) == kSmiTag);
  value = value >> kSmiTagShift;
  if ((value <= Smi::kMaxValue) && (value >= Smi::kMinValue)) {
    return Smi::New(value);
  }
  if (kind_ == Snapshot::kFull) {
    return NewMint(value);
  }
  return Mint::NewCanonical(value);
}


RawStacktrace* SnapshotReader::NewStacktrace() {
  ALLOC_NEW_OBJECT(Stacktrace, object_store()->stacktrace_class());
}


RawClass* SnapshotReader::LookupInternalClass(intptr_t class_header) {
  // If the header is an object Id, lookup singleton VM classes or classes
  // stored in the object store.
  if (IsVMIsolateObject(class_header)) {
    intptr_t class_id = GetVMIsolateObjectId(class_header);
    if (IsSingletonClassId(class_id)) {
      return isolate()->class_table()->At(class_id);  // get singleton class.
    }
  } else if (SerializedHeaderTag::decode(class_header) == kObjectId) {
    intptr_t class_id = SerializedHeaderData::decode(class_header);
    if (IsObjectStoreClassId(class_id)) {
      return isolate()->class_table()->At(class_id);  // get singleton class.
    }
  }
  return Class::null();
}


RawObject* SnapshotReader::AllocateUninitialized(const Class& cls,
                                                 intptr_t size) {
  ASSERT(isolate()->no_gc_scope_depth() != 0);
  ASSERT(Utils::IsAligned(size, kObjectAlignment));
  Heap* heap = isolate()->heap();

  uword address = heap->TryAllocate(size, Heap::kOld);
  if (address == 0) {
    // Use the preallocated out of memory exception to avoid calling
    // into dart code or allocating any code.
    // We do a longjmp at this point to unwind out of the entire
    // read part and return the error object back.
    const Instance& exception =
        Instance::Handle(object_store()->out_of_memory());
    ErrorHandle()->set_exception(exception);
    Isolate::Current()->long_jump_base()->Jump(1, *ErrorHandle());
  }
  RawObject* raw_obj = reinterpret_cast<RawObject*>(address + kHeapObjectTag);
  uword tags = 0;
  intptr_t index = cls.id();
  ASSERT(index != kIllegalCid);
  tags = RawObject::ClassIdTag::update(index, tags);
  tags = RawObject::SizeTag::update(size, tags);
  raw_obj->ptr()->tags_ = tags;
  return raw_obj;
}


RawObject* SnapshotReader::ReadVMIsolateObject(intptr_t header_value) {
  intptr_t object_id = GetVMIsolateObjectId(header_value);
  if (object_id == kNullObject) {
    // This is a singleton null object, return it.
    return Object::null();
  }
  if (object_id == kSentinelObject) {
    return Object::sentinel().raw();
  }
  if (object_id == kEmptyArrayObject) {
    return Object::empty_array().raw();
  }
  if (object_id == kTrueValue) {
    return Bool::True().raw();
  }
  if (object_id == kFalseValue) {
    return Bool::False().raw();
  }
  intptr_t class_id = ClassIdFromObjectId(object_id);
  if (IsSingletonClassId(class_id)) {
    return isolate()->class_table()->At(class_id);  // get singleton class.
  } else {
    ASSERT(Symbols::IsVMSymbolId(object_id));
    return Symbols::GetVMSymbol(object_id);  // return VM symbol.
  }
  UNREACHABLE();
  return Object::null();
}


RawObject* SnapshotReader::ReadIndexedObject(intptr_t object_id) {
  intptr_t class_id = ClassIdFromObjectId(object_id);
  if (IsObjectStoreClassId(class_id)) {
    return isolate()->class_table()->At(class_id);  // get singleton class.
  }
  if (kind_ != Snapshot::kFull) {
    if (IsObjectStoreTypeId(object_id)) {
      return GetType(object_store(), object_id);  // return type obj.
    }
  }
  Object* object = GetBackRef(object_id);
  return object->raw();
}


RawObject* SnapshotReader::ReadInlinedObject(intptr_t object_id) {
  // Read the class header information and lookup the class.
  intptr_t class_header = ReadIntptrValue();
  intptr_t tags = ReadIntptrValue();
  if (SerializedHeaderData::decode(class_header) == kInstanceObjectId) {
    // Object is regular dart instance.
    Instance* result = reinterpret_cast<Instance*>(GetBackRef(object_id));
    intptr_t instance_size = 0;
    if (result == NULL) {
      result = &(Instance::ZoneHandle(isolate(), Instance::null()));
      AddBackRef(object_id, result, kIsDeserialized);
      cls_ ^= ReadObjectImpl();
      ASSERT(!cls_.IsNull());
      instance_size = cls_.instance_size();
      ASSERT(instance_size > 0);
      // Allocate the instance and read in all the fields for the object.
      if (kind_ == Snapshot::kFull) {
        *result ^= AllocateUninitialized(cls_, instance_size);
      } else {
        *result ^= Object::Allocate(cls_.id(),
                                    instance_size,
                                    HEAP_SPACE(kind_));
      }
    } else {
      cls_ ^= ReadObjectImpl();
      ASSERT(!cls_.IsNull());
      instance_size = cls_.instance_size();
    }
    intptr_t offset = Object::InstanceSize();
    while (offset < instance_size) {
      obj_ = ReadObjectRef();
      result->SetFieldAtOffset(offset, obj_);
      offset += kWordSize;
    }
    if (kind_ == Snapshot::kFull) {
      result->SetCreatedFromSnapshot();
    } else if (result->IsCanonical()) {
      *result = result->Canonicalize();
    }
    return result->raw();
  }
  ASSERT((class_header & kSmiTagMask) != kSmiTag);
  cls_ = LookupInternalClass(class_header);
  ASSERT(!cls_.IsNull());
  switch (cls_.id()) {
#define SNAPSHOT_READ(clazz)                                                   \
    case clazz::kClassId: {                                                    \
      obj_ = clazz::ReadFrom(this, object_id, tags, kind_);                    \
      break;                                                                   \
    }
    CLASS_LIST_NO_OBJECT(SNAPSHOT_READ)
#undef SNAPSHOT_READ
#define SNAPSHOT_READ(clazz)                                                   \
    case kTypedData##clazz##Cid:                                               \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_READ) {
      obj_ = TypedData::ReadFrom(this, object_id, tags, kind_);
      break;
    }
#undef SNAPSHOT_READ
#define SNAPSHOT_READ(clazz)                                                   \
    case kExternalTypedData##clazz##Cid:                                       \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_READ) {
      obj_ = ExternalTypedData::ReadFrom(this, object_id, tags, kind_);
      break;
    }
#undef SNAPSHOT_READ
    default: UNREACHABLE(); break;
  }
  if (kind_ == Snapshot::kFull) {
    obj_.SetCreatedFromSnapshot();
  }
  return obj_.raw();
}


void SnapshotReader::ArrayReadFrom(const Array& result,
                                   intptr_t len,
                                   intptr_t tags) {
  // Set the object tags.
  result.set_tags(tags);

  // Setup the object fields.
  *TypeArgumentsHandle() ^= ReadObjectImpl();
  result.SetTypeArguments(*TypeArgumentsHandle());

  for (intptr_t i = 0; i < len; i++) {
    *ObjectHandle() = ReadObjectRef();
    result.SetAt(i, *ObjectHandle());
  }
}


SnapshotWriter::SnapshotWriter(Snapshot::Kind kind,
                               uint8_t** buffer,
                               ReAlloc alloc,
                               intptr_t initial_size)
    : BaseWriter(buffer, alloc, initial_size),
      kind_(kind),
      object_store_(Isolate::Current()->object_store()),
      class_table_(Isolate::Current()->class_table()),
      forward_list_(),
      exception_type_(Exceptions::kNone),
      exception_msg_(NULL),
      error_(LanguageError::Handle()) {
}


void SnapshotWriter::WriteObject(RawObject* rawobj) {
  WriteObjectImpl(rawobj);
  WriteForwardedObjects();
}


void SnapshotWriter::HandleVMIsolateObject(RawObject* rawobj) {
  // Check if it is a singleton null object.
  if (rawobj == Object::null()) {
    WriteVMIsolateObject(kNullObject);
    return;
  }

  // Check if it is a singleton sentinel object.
  if (rawobj == Object::sentinel().raw()) {
    WriteVMIsolateObject(kSentinelObject);
    return;
  }

  // Check if it is a singleton empty array object.
  if (rawobj == Object::empty_array().raw()) {
    WriteVMIsolateObject(kEmptyArrayObject);
    return;
  }

  // Check if it is a singleton boolean true object.
  if (rawobj == Bool::True().raw()) {
    WriteVMIsolateObject(kTrueValue);
    return;
  }

  // Check if it is a singleton boolean false object.
  if (rawobj == Bool::False().raw()) {
    WriteVMIsolateObject(kFalseValue);
    return;
  }

  // Check if it is a singleton class object which is shared by
  // all isolates.
  intptr_t id = rawobj->GetClassId();
  if (id == kClassCid) {
    RawClass* raw_class = reinterpret_cast<RawClass*>(rawobj);
    intptr_t class_id = raw_class->ptr()->id_;
    if (IsSingletonClassId(class_id)) {
      intptr_t object_id = ObjectIdFromClassId(class_id);
      WriteVMIsolateObject(object_id);
      return;
    }
  }

  // Check it is a predefined symbol in the VM isolate.
  id = Symbols::LookupVMSymbol(rawobj);
  if (id != kInvalidIndex) {
    WriteVMIsolateObject(id);
    return;
  }

  UNREACHABLE();
}


void SnapshotWriter::WriteObjectRef(RawObject* raw) {
  // First check if object can be written as a simple predefined type.
  if (CheckAndWritePredefinedObject(raw)) {
    return;
  }

  NoGCScope no_gc;
  RawClass* cls = class_table_->At(raw->GetClassId());
  intptr_t class_id = cls->ptr()->id_;
  ASSERT(class_id == raw->GetClassId());
  if (class_id >= kNumPredefinedCids) {
    WriteInstanceRef(raw, cls);
    return;
  }
  if (class_id == kArrayCid) {
    // Object is being referenced, add it to the forward ref list and mark
    // it so that future references to this object in the snapshot will use
    // this object id. Mark it as not having been serialized yet so that we
    // will serialize the object when we go through the forward list.
    intptr_t object_id = MarkObject(raw, kIsNotSerialized);

    RawArray* rawarray = reinterpret_cast<RawArray*>(raw);

    // Write out the serialization header value for this object.
    WriteInlinedObjectHeader(object_id);

    // Write out the class information.
    WriteIndexedObject(kArrayCid);

    // Write out the length field.
    Write<RawObject*>(rawarray->ptr()->length_);

    return;
  }
  if (class_id == kImmutableArrayCid) {
    // Object is being referenced, add it to the forward ref list and mark
    // it so that future references to this object in the snapshot will use
    // this object id. Mark it as not having been serialized yet so that we
    // will serialize the object when we go through the forward list.
    intptr_t object_id = MarkObject(raw, kIsNotSerialized);

    RawArray* rawarray = reinterpret_cast<RawArray*>(raw);

    // Write out the serialization header value for this object.
    WriteInlinedObjectHeader(object_id);

    // Write out the class information.
    WriteIndexedObject(kImmutableArrayCid);

    // Write out the length field.
    Write<RawObject*>(rawarray->ptr()->length_);

    return;
  }
  if (RawObject::IsTypedDataViewClassId(class_id)) {
    WriteInstanceRef(raw, cls);
    return;
  }
  // Object is being referenced, add it to the forward ref list and mark
  // it so that future references to this object in the snapshot will use
  // this object id. Mark it as not having been serialized yet so that we
  // will serialize the object when we go through the forward list.
  intptr_t object_id = MarkObject(raw, kIsSerialized);
  switch (class_id) {
#define SNAPSHOT_WRITE(clazz)                                                  \
    case clazz::kClassId: {                                                    \
      Raw##clazz* raw_obj = reinterpret_cast<Raw##clazz*>(raw);                \
      raw_obj->WriteTo(this, object_id, kind_);                                \
      return;                                                                  \
    }                                                                          \

    CLASS_LIST_NO_OBJECT(SNAPSHOT_WRITE)
#undef SNAPSHOT_WRITE
#define SNAPSHOT_WRITE(clazz)                                                  \
    case kTypedData##clazz##Cid:                                               \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_WRITE) {
      RawTypedData* raw_obj = reinterpret_cast<RawTypedData*>(raw);
      raw_obj->WriteTo(this, object_id, kind_);
      return;
    }
#undef SNAPSHOT_WRITE
#define SNAPSHOT_WRITE(clazz)                                                  \
    case kExternalTypedData##clazz##Cid:                                       \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_WRITE) {
      RawExternalTypedData* raw_obj =
        reinterpret_cast<RawExternalTypedData*>(raw);
      raw_obj->WriteTo(this, object_id, kind_);
      return;
    }
#undef SNAPSHOT_WRITE
    default: break;
  }
  UNREACHABLE();
}


void FullSnapshotWriter::WriteFullSnapshot() {
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  ObjectStore* object_store = isolate->object_store();
  ASSERT(object_store != NULL);
  ASSERT(ClassFinalizer::AllClassesFinalized());

  // Setup for long jump in case there is an exception while writing
  // the snapshot.
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  // TODO(6726): Allocate these constant strings once in the VM isolate.
  *ErrorHandle() = LanguageError::New(
      String::Handle(String::New("Error while writing full snapshot")));
  if (setjmp(*jump.Set()) == 0) {
    NoGCScope no_gc;

    // Reserve space in the output buffer for a snapshot header.
    ReserveHeader();

    // Write out all the objects in the object store of the isolate which
    // is the root set for all dart allocated objects at this point.
    SnapshotWriterVisitor visitor(this, false);
    object_store->VisitObjectPointers(&visitor);

    // Write out all forwarded objects.
    WriteForwardedObjects();

    FillHeader(kind());
    UnmarkAll();

    isolate->set_long_jump_base(base);
  } else {
    isolate->set_long_jump_base(base);
    ThrowException(exception_type(), exception_msg());
  }
}


uword SnapshotWriter::GetObjectTags(RawObject* raw) {
  uword tags = raw->ptr()->tags_;
  if (SerializedHeaderTag::decode(tags) == kObjectId) {
    intptr_t id = SerializedHeaderData::decode(tags);
    return forward_list_[id - kMaxPredefinedObjectIds]->tags();
  } else {
    return tags;
  }
}


intptr_t SnapshotWriter::MarkObject(RawObject* raw, SerializeState state) {
  NoGCScope no_gc;
  intptr_t object_id = forward_list_.length() + kMaxPredefinedObjectIds;
  ASSERT(object_id <= kMaxObjectId);
  uword value = 0;
  value = SerializedHeaderTag::update(kObjectId, value);
  value = SerializedHeaderData::update(object_id, value);
  uword tags = raw->ptr()->tags_;
  ASSERT(SerializedHeaderTag::decode(tags) != kObjectId);
  raw->ptr()->tags_ = value;
  ForwardObjectNode* node = new ForwardObjectNode(raw, tags, state);
  ASSERT(node != NULL);
  forward_list_.Add(node);
  return object_id;
}


void SnapshotWriter::UnmarkAll() {
  NoGCScope no_gc;
  for (intptr_t i = 0; i < forward_list_.length(); i++) {
    RawObject* raw = forward_list_[i]->raw();
    raw->ptr()->tags_ = forward_list_[i]->tags();  // Restore original tags.
  }
}


bool SnapshotWriter::CheckAndWritePredefinedObject(RawObject* rawobj) {
  // Check if object can be written in one of the following ways:
  // - Smi: the Smi value is written as is (last bit is not tagged).
  // - VM internal class (from VM isolate): (index of class in vm isolate | 0x3)
  // - Object that has already been written: (negative id in stream | 0x3)

  NoGCScope no_gc;

  // First check if it is a Smi (i.e not a heap object).
  if (!rawobj->IsHeapObject()) {
    Write<int64_t>(reinterpret_cast<intptr_t>(rawobj));
    return true;
  }

  // Check if object has already been serialized, in that case just write
  // the object id out.
  uword tags = rawobj->ptr()->tags_;
  if (SerializedHeaderTag::decode(tags) == kObjectId) {
    intptr_t id = SerializedHeaderData::decode(tags);
    WriteIndexedObject(id);
    return true;
  }

  // Now check if it is an object from the VM isolate (NOTE: premarked objects
  // are considered to be objects in the VM isolate). These objects are shared
  // by all isolates.
  if (rawobj->IsVMHeapObject()) {
    HandleVMIsolateObject(rawobj);
    return true;
  }

  // Check if the object is a Mint and could potentially be a Smi
  // on other architectures (64 bit), if so write it out as int64_t value.
  if (rawobj->GetClassId() == kMintCid) {
    int64_t value = reinterpret_cast<RawMint*>(rawobj)->ptr()->value_;
    const intptr_t kSmi64Bits = 62;
    const int64_t kSmi64Max = (static_cast<int64_t>(1) << kSmi64Bits) - 1;
    const int64_t kSmi64Min = -(static_cast<int64_t>(1) << kSmi64Bits);
    if (value <= kSmi64Max && value >= kSmi64Min) {
      Write<int64_t>((value << kSmiTagShift) | kSmiTag);
      return true;
    }
  }

  // Check if it is a code object in that case just write a Null object
  // as we do not want code objects in the snapshot.
  if (rawobj->GetClassId() == kCodeCid) {
    WriteVMIsolateObject(kNullObject);
    return true;
  }

  // Check if classes are not being serialized and it is preinitialized type.
  if (kind_ != Snapshot::kFull) {
    RawType* raw_type = reinterpret_cast<RawType*>(rawobj);
    intptr_t index = GetTypeIndex(object_store(), raw_type);
    if (index != kInvalidIndex) {
      WriteIndexedObject(index);
      return true;
    }
  }

  return false;
}


void SnapshotWriter::WriteObjectImpl(RawObject* raw) {
  // First check if object can be written as a simple predefined type.
  if (CheckAndWritePredefinedObject(raw)) {
    return;
  }

  // Object is being serialized, add it to the forward ref list and mark
  // it so that future references to this object in the snapshot will use
  // an object id, instead of trying to serialize it again.
  MarkObject(raw, kIsSerialized);

  WriteInlinedObject(raw);
}


void SnapshotWriter::WriteInlinedObject(RawObject* raw) {
  // Now write the object out inline in the stream as follows:
  // - Object is seen for the first time (inlined as follows):
  //    (object size in multiples of kObjectAlignment | 0x1)
  //    serialized fields of the object
  //    ......
  NoGCScope no_gc;
  uword tags = raw->ptr()->tags_;
  ASSERT(SerializedHeaderTag::decode(tags) == kObjectId);
  intptr_t object_id = SerializedHeaderData::decode(tags);
  tags = forward_list_[object_id - kMaxPredefinedObjectIds]->tags();
  RawClass* cls = class_table_->At(RawObject::ClassIdTag::decode(tags));
  intptr_t class_id = cls->ptr()->id_;

  if (class_id >= kNumPredefinedCids) {
    WriteInstance(object_id, raw, cls, tags);
    return;
  }
  switch (class_id) {
#define SNAPSHOT_WRITE(clazz)                                                  \
    case clazz::kClassId: {                                                    \
      Raw##clazz* raw_obj = reinterpret_cast<Raw##clazz*>(raw);                \
      raw_obj->WriteTo(this, object_id, kind_);                                \
      return;                                                                  \
    }                                                                          \

    CLASS_LIST_NO_OBJECT(SNAPSHOT_WRITE)
#undef SNAPSHOT_WRITE
#define SNAPSHOT_WRITE(clazz)                                                  \
    case kTypedData##clazz##Cid:                                               \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_WRITE) {
      RawTypedData* raw_obj = reinterpret_cast<RawTypedData*>(raw);
      raw_obj->WriteTo(this, object_id, kind_);
      return;
    }
#undef SNAPSHOT_WRITE
#define SNAPSHOT_WRITE(clazz)                                                  \
    case kExternalTypedData##clazz##Cid:                                       \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_WRITE) {
      RawExternalTypedData* raw_obj =
        reinterpret_cast<RawExternalTypedData*>(raw);
      raw_obj->WriteTo(this, object_id, kind_);
      return;
    }
#undef SNAPSHOT_WRITE
#define SNAPSHOT_WRITE(clazz)                                                  \
    case kTypedData##clazz##ViewCid:                                           \

    CLASS_LIST_TYPED_DATA(SNAPSHOT_WRITE)
    case kByteDataViewCid: {
      WriteInstance(object_id, raw, cls, tags);
      return;
    }
#undef SNAPSHOT_WRITE
    default: break;
  }
  UNREACHABLE();
}


void SnapshotWriter::WriteForwardedObjects() {
  // Write out all objects that were added to the forward list and have
  // not been serialized yet. These would typically be fields of instance
  // objects, arrays or immutable arrays (this is done in order to avoid
  // deep recursive calls to WriteObjectImpl).
  // NOTE: The forward list might grow as we process the list.
  for (intptr_t i = 0; i < forward_list_.length(); i++) {
    if (!forward_list_[i]->is_serialized()) {
      // Write the object out in the stream.
      RawObject* raw = forward_list_[i]->raw();
      WriteInlinedObject(raw);

      // Mark object as serialized.
      forward_list_[i]->set_state(kIsSerialized);
    }
  }
}


void SnapshotWriter::WriteClassId(RawClass* cls) {
  ASSERT(kind_ != Snapshot::kFull);
  int class_id = cls->ptr()->id_;
  if (IsSingletonClassId(class_id)) {
    intptr_t object_id = ObjectIdFromClassId(class_id);
    WriteVMIsolateObject(object_id);
  } else if (IsObjectStoreClassId(class_id)) {
    intptr_t object_id = ObjectIdFromClassId(class_id);
    WriteIndexedObject(object_id);
  } else {
    // TODO(5411462): Should restrict this to only core-lib classes in this
    // case.
    // Write out the class and tags information.
    WriteVMIsolateObject(kClassCid);
    WriteIntptrValue(GetObjectTags(cls));

    // Write out the library url and class name.
    RawLibrary* library = cls->ptr()->library_;
    ASSERT(library != Library::null());
    WriteObjectImpl(library->ptr()->url_);
    WriteObjectImpl(cls->ptr()->name_);
  }
}


void SnapshotWriter::ArrayWriteTo(intptr_t object_id,
                                  intptr_t array_kind,
                                  intptr_t tags,
                                  RawSmi* length,
                                  RawAbstractTypeArguments* type_arguments,
                                  RawObject* data[]) {
  intptr_t len = Smi::Value(length);

  // Write out the serialization header value for this object.
  WriteInlinedObjectHeader(object_id);

  // Write out the class and tags information.
  WriteIndexedObject(array_kind);
  WriteIntptrValue(tags);

  // Write out the length field.
  Write<RawObject*>(length);

  // Write out the type arguments.
  WriteObjectImpl(type_arguments);

  // Write out the individual object ids.
  for (intptr_t i = 0; i < len; i++) {
    WriteObjectRef(data[i]);
  }
}


void SnapshotWriter::CheckIfSerializable(RawClass* cls) {
  if (Class::IsSignatureClass(cls)) {
    // We do not allow closure objects in an isolate message.
    set_exception_type(Exceptions::kArgument);
    // TODO(6726): Allocate these constant strings once in the VM isolate.
    set_exception_msg("Illegal argument in isolate message"
                      " : (object is a closure)");
    Isolate::Current()->long_jump_base()->Jump(1, *ErrorHandle());
  }
  if (cls->ptr()->num_native_fields_ != 0) {
    // We do not allow objects with native fields in an isolate message.
    set_exception_type(Exceptions::kArgument);
    // TODO(6726): Allocate these constant strings once in the VM isolate.
    set_exception_msg("Illegal argument in isolate message"
                      " : (object extends NativeWrapper)");

    Isolate::Current()->long_jump_base()->Jump(1, *ErrorHandle());
  }
}


void SnapshotWriter::WriteInstance(intptr_t object_id,
                                   RawObject* raw,
                                   RawClass* cls,
                                   intptr_t tags) {
  // First check if object is a closure or has native fields.
  CheckIfSerializable(cls);

  // Object is regular dart instance.
  intptr_t instance_size =
      cls->ptr()->instance_size_in_words_ << kWordSizeLog2;
  ASSERT(instance_size != 0);

  // Write out the serialization header value for this object.
  WriteInlinedObjectHeader(object_id);

  // Indicate this is an instance object.
  WriteIntptrValue(SerializedHeaderData::encode(kInstanceObjectId));

  // Write out the tags.
  WriteIntptrValue(tags);

  // Write out the class information for this object.
  WriteObjectImpl(cls);

  // Write out all the fields for the object.
  intptr_t offset = Object::InstanceSize();
  while (offset < instance_size) {
    WriteObjectRef(*reinterpret_cast<RawObject**>(
        reinterpret_cast<uword>(raw->ptr()) + offset));
    offset += kWordSize;
  }
  return;
}


void SnapshotWriter::WriteInstanceRef(RawObject* raw, RawClass* cls) {
  // First check if object is a closure or has native fields.
  CheckIfSerializable(cls);

  // Object is being referenced, add it to the forward ref list and mark
  // it so that future references to this object in the snapshot will use
  // this object id. Mark it as not having been serialized yet so that we
  // will serialize the object when we go through the forward list.
  intptr_t object_id = MarkObject(raw, kIsNotSerialized);

  // Write out the serialization header value for this object.
  WriteInlinedObjectHeader(object_id);

  // Indicate this is an instance object.
  WriteIntptrValue(SerializedHeaderData::encode(kInstanceObjectId));

  // Write out the class information for this object.
  WriteObjectImpl(cls);
}


void SnapshotWriter::ThrowException(Exceptions::ExceptionType type,
                                    const char* msg) {
  Isolate::Current()->object_store()->clear_sticky_error();
  UnmarkAll();
  const String& msg_obj = String::Handle(String::New(msg));
  const Array& args = Array::Handle(Array::New(1));
  args.SetAt(0, msg_obj);
  Exceptions::ThrowByType(type, args);
  UNREACHABLE();
}


void ScriptSnapshotWriter::WriteScriptSnapshot(const Library& lib) {
  ASSERT(kind() == Snapshot::kScript);
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);
  ASSERT(ClassFinalizer::AllClassesFinalized());

  // Setup for long jump in case there is an exception while writing
  // the snapshot.
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  *ErrorHandle() = LanguageError::New(
      String::Handle(String::New("Error while writing script snapshot")));
  if (setjmp(*jump.Set()) == 0) {
    // Write out the library object.
    NoGCScope no_gc;
    ReserveHeader();
    WriteObject(lib.raw());
    FillHeader(kind());
    UnmarkAll();
    isolate->set_long_jump_base(base);
  } else {
    isolate->set_long_jump_base(base);
    ThrowException(exception_type(), exception_msg());
  }
}


void SnapshotWriterVisitor::VisitPointers(RawObject** first, RawObject** last) {
  for (RawObject** current = first; current <= last; current++) {
    RawObject* raw_obj = *current;
    if (as_references_) {
      writer_->WriteObjectRef(raw_obj);
    } else {
      writer_->WriteObjectImpl(raw_obj);
    }
  }
}


void MessageWriter::WriteMessage(const Object& obj) {
  ASSERT(kind() == Snapshot::kMessage);
  Isolate* isolate = Isolate::Current();
  ASSERT(isolate != NULL);

  // Setup for long jump in case there is an exception while writing
  // the message.
  LongJump* base = isolate->long_jump_base();
  LongJump jump;
  isolate->set_long_jump_base(&jump);
  *ErrorHandle() = LanguageError::New(
      String::Handle(String::New("Error while writing message")));
  if (setjmp(*jump.Set()) == 0) {
    NoGCScope no_gc;
    WriteObject(obj.raw());
    UnmarkAll();
    isolate->set_long_jump_base(base);
  } else {
    isolate->set_long_jump_base(base);
    ThrowException(exception_type(), exception_msg());
  }
}


}  // namespace dart
