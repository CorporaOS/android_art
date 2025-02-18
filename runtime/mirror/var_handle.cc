/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "var_handle.h"

#include "array-inl.h"
#include "art_field-inl.h"
#include "base/casts.h"
#include "class-inl.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "intrinsics_enum.h"
#include "jni/jni_internal.h"
#include "jvalue-inl.h"
#include "method_handles-inl.h"
#include "method_type-inl.h"
#include "object_array-alloc-inl.h"
#include "obj_ptr-inl.h"
#include "well_known_classes.h"

namespace art HIDDEN {
namespace mirror {

static constexpr bool kTransactionActive = true;
static constexpr bool kTransactionInactive = !kTransactionActive;

namespace {

struct VarHandleAccessorToAccessModeEntry {
  const char* method_name;
  VarHandle::AccessMode access_mode;

  // Binary predicate function for finding access_mode by
  // method_name. The access_mode field is ignored.
  static bool CompareName(const VarHandleAccessorToAccessModeEntry& lhs,
                          const VarHandleAccessorToAccessModeEntry& rhs) {
    return strcmp(lhs.method_name, rhs.method_name) < 0;
  }
};

// Map of VarHandle accessor method names to access mode values. The list is alpha-sorted to support
// binary search. For the usage scenario - lookups in the verifier - a linear scan would likely
// suffice since we expect VarHandles to be a lesser encountered class. We could use a std::hashmap
// here and this would be easier to maintain if new values are added here. However, this entails
// CPU cycles initializing the structure on every execution and uses O(N) more memory for
// intermediate nodes and makes that memory dirty. Compile-time magic using constexpr is possible
// here, but that's a tax when this code is recompiled.
const VarHandleAccessorToAccessModeEntry kAccessorToAccessMode[VarHandle::kNumberOfAccessModes] = {
  { "compareAndExchange", VarHandle::AccessMode::kCompareAndExchange },
  { "compareAndExchangeAcquire", VarHandle::AccessMode::kCompareAndExchangeAcquire },
  { "compareAndExchangeRelease", VarHandle::AccessMode::kCompareAndExchangeRelease },
  { "compareAndSet", VarHandle::AccessMode::kCompareAndSet },
  { "get", VarHandle::AccessMode::kGet },
  { "getAcquire", VarHandle::AccessMode::kGetAcquire },
  { "getAndAdd", VarHandle::AccessMode::kGetAndAdd },
  { "getAndAddAcquire", VarHandle::AccessMode::kGetAndAddAcquire },
  { "getAndAddRelease", VarHandle::AccessMode::kGetAndAddRelease },
  { "getAndBitwiseAnd", VarHandle::AccessMode::kGetAndBitwiseAnd },
  { "getAndBitwiseAndAcquire", VarHandle::AccessMode::kGetAndBitwiseAndAcquire },
  { "getAndBitwiseAndRelease", VarHandle::AccessMode::kGetAndBitwiseAndRelease },
  { "getAndBitwiseOr", VarHandle::AccessMode::kGetAndBitwiseOr },
  { "getAndBitwiseOrAcquire", VarHandle::AccessMode::kGetAndBitwiseOrAcquire },
  { "getAndBitwiseOrRelease", VarHandle::AccessMode::kGetAndBitwiseOrRelease },
  { "getAndBitwiseXor", VarHandle::AccessMode::kGetAndBitwiseXor },
  { "getAndBitwiseXorAcquire", VarHandle::AccessMode::kGetAndBitwiseXorAcquire },
  { "getAndBitwiseXorRelease", VarHandle::AccessMode::kGetAndBitwiseXorRelease },
  { "getAndSet", VarHandle::AccessMode::kGetAndSet },
  { "getAndSetAcquire", VarHandle::AccessMode::kGetAndSetAcquire },
  { "getAndSetRelease", VarHandle::AccessMode::kGetAndSetRelease },
  { "getOpaque", VarHandle::AccessMode::kGetOpaque },
  { "getVolatile", VarHandle::AccessMode::kGetVolatile },
  { "set", VarHandle::AccessMode::kSet },
  { "setOpaque", VarHandle::AccessMode::kSetOpaque },
  { "setRelease", VarHandle::AccessMode::kSetRelease },
  { "setVolatile", VarHandle::AccessMode::kSetVolatile },
  { "weakCompareAndSet", VarHandle::AccessMode::kWeakCompareAndSet },
  { "weakCompareAndSetAcquire", VarHandle::AccessMode::kWeakCompareAndSetAcquire },
  { "weakCompareAndSetPlain", VarHandle::AccessMode::kWeakCompareAndSetPlain },
  { "weakCompareAndSetRelease", VarHandle::AccessMode::kWeakCompareAndSetRelease },
};

// Returns the number of parameters associated with an
// AccessModeTemplate and the supplied coordinate types.
int32_t GetNumberOfParameters(VarHandle::AccessModeTemplate access_mode_template,
                              ObjPtr<Class> coordinateType0,
                              ObjPtr<Class> coordinateType1) {
  int32_t count = 0;
  if (!coordinateType0.IsNull()) {
    count++;
    if (!coordinateType1.IsNull()) {
      count++;
    }
  }
  return count + VarHandle::GetNumberOfVarTypeParameters(access_mode_template);
}

void ThrowNullPointerExceptionForCoordinate() REQUIRES_SHARED(Locks::mutator_lock_) {
  ThrowNullPointerException("Attempt to access memory on a null object");
}

bool CheckElementIndex(Primitive::Type type,
                       int32_t index,
                       int32_t start,
                       int32_t length) REQUIRES_SHARED(Locks::mutator_lock_) {
  // The underlying memory may be shared and offset from the start of allocated region,
  // ie buffers can be created via ByteBuffer.split().
  //
  // `type` is the type of the value the caller is attempting to read / write.
  // `index` represents the position the caller is trying to access in the underlying ByteBuffer
  //         or byte array. This is an offset from from `start` in bytes.
  // `start` represents where the addressable memory begins relative to the base of the
  //         the underlying ByteBuffer or byte array.
  // `length` represents the length of the addressable region.
  //
  // Thus the region being operated on is:
  //    `base` + `start` + `index` to `base` + `start` + `index` + `sizeof(type)`
  int32_t max_index = length - start - Primitive::ComponentSize(type);
  if (index < 0 || index > max_index) {
    ThrowIndexOutOfBoundsException(index, length - start);
    return false;
  }
  return true;
}

bool CheckElementIndex(Primitive::Type type, int32_t index, int32_t length)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return CheckElementIndex(type, index, 0, length);
}

// Returns true if access_mode only entails a memory read. False if
// access_mode may write to memory.
bool IsReadOnlyAccessMode(VarHandle::AccessMode access_mode) {
  return VarHandle::GetAccessModeTemplate(access_mode) == VarHandle::AccessModeTemplate::kGet;
}

// Writes the parameter types associated with the AccessModeTemplate
// into an array. The parameter types are derived from the specified
// variable type and coordinate types. Returns the number of
// parameters written.
int32_t BuildParameterArray(ObjPtr<Class> (&parameters)[VarHandle::kMaxAccessorParameters],
                            VarHandle::AccessModeTemplate access_mode_template,
                            ObjPtr<Class> varType,
                            ObjPtr<Class> coordinateType0,
                            ObjPtr<Class> coordinateType1)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(varType != nullptr);
  int32_t index = 0;
  if (!coordinateType0.IsNull()) {
    parameters[index++] = coordinateType0;
    if (!coordinateType1.IsNull()) {
      parameters[index++] = coordinateType1;
    }
  } else {
    DCHECK(coordinateType1.IsNull());
  }

  switch (access_mode_template) {
    case VarHandle::AccessModeTemplate::kCompareAndExchange:
    case VarHandle::AccessModeTemplate::kCompareAndSet:
      parameters[index++] = varType;
      parameters[index++] = varType;
      return index;
    case VarHandle::AccessModeTemplate::kGet:
      return index;
    case VarHandle::AccessModeTemplate::kGetAndUpdate:
    case VarHandle::AccessModeTemplate::kSet:
      parameters[index++] = varType;
      return index;
  }
  return -1;
}

// Returns the return type associated with an VarHandle::AccessModeTemplate based
// on the template and the variable type specified.
static ObjPtr<Class> GetReturnType(VarHandle::AccessModeTemplate access_mode_template,
                                   ObjPtr<Class> varType)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(varType != nullptr);
  switch (access_mode_template) {
    case VarHandle::AccessModeTemplate::kCompareAndSet:
      return GetClassRoot(ClassRoot::kPrimitiveBoolean);
    case VarHandle::AccessModeTemplate::kCompareAndExchange:
    case VarHandle::AccessModeTemplate::kGet:
    case VarHandle::AccessModeTemplate::kGetAndUpdate:
      return varType;
    case VarHandle::AccessModeTemplate::kSet:
      return GetClassRoot(ClassRoot::kPrimitiveVoid);
  }
  return nullptr;
}

// Method to insert a read barrier for accessors to reference fields.
inline void ReadBarrierForVarHandleAccess(ObjPtr<Object> obj, MemberOffset field_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (gUseReadBarrier) {
    // We need to ensure that the reference stored in the field is a to-space one before attempting
    // the CompareAndSet/CompareAndExchange/Exchange operation otherwise it will fail incorrectly
    // if obj is in the process of being moved.
    uint8_t* raw_field_addr = reinterpret_cast<uint8_t*>(obj.Ptr()) + field_offset.SizeValue();
    auto field_addr = reinterpret_cast<mirror::HeapReference<mirror::Object>*>(raw_field_addr);
    // Note that the read barrier load does NOT need to be volatile.
    static constexpr bool kIsVolatile = false;
    static constexpr bool kAlwaysUpdateField = true;
    ReadBarrier::Barrier<mirror::Object, kIsVolatile, kWithReadBarrier, kAlwaysUpdateField>(
        obj.Ptr(),
        MemberOffset(field_offset),
        field_addr);
  }
}

//
// Helper methods for storing results from atomic operations into
// JValue instances.
//

inline void StoreResult(uint8_t value, JValue* result) {
  result->SetZ(value);
}

inline void StoreResult(int8_t value, JValue* result) {
  result->SetB(value);
}

inline void StoreResult(uint16_t value, JValue* result) {
  result->SetC(value);
}

inline void StoreResult(int16_t value, JValue* result) {
  result->SetS(value);
}

inline void StoreResult(int32_t value, JValue* result) {
  result->SetI(value);
}

inline void StoreResult(int64_t value, JValue* result) {
  result->SetJ(value);
}

inline void StoreResult(float value, JValue* result) {
  result->SetF(value);
}

inline void StoreResult(double value, JValue* result) {
  result->SetD(value);
}

inline void StoreResult(ObjPtr<Object> value, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  result->SetL(value);
}

//
// Helper class for byte-swapping value that has been stored in a JValue.
//

template <typename T>
class JValueByteSwapper final {
 public:
  static void ByteSwap(JValue* value);
  static void MaybeByteSwap(bool byte_swap, JValue* value) {
    if (byte_swap) {
      ByteSwap(value);
    }
  }
};

template <>
void JValueByteSwapper<uint16_t>::ByteSwap(JValue* value) {
  value->SetC(BSWAP(value->GetC()));
}

template <>
void JValueByteSwapper<int16_t>::ByteSwap(JValue* value) {
  value->SetS(BSWAP(value->GetS()));
}

template <>
void JValueByteSwapper<int32_t>::ByteSwap(JValue* value) {
  value->SetI(BSWAP(value->GetI()));
}

template <>
void JValueByteSwapper<int64_t>::ByteSwap(JValue* value) {
  value->SetJ(BSWAP(value->GetJ()));
}

//
// Accessor implementations, shared across all VarHandle types.
//

template <typename T, std::memory_order MO>
class AtomicGetAccessor : public Object::Accessor<T> {
 public:
  explicit AtomicGetAccessor(JValue* result) : result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    StoreResult(atom->load(MO), result_);
  }

 private:
  JValue* result_;
};

template <typename T, std::memory_order MO>
class AtomicSetAccessor : public Object::Accessor<T> {
 public:
  explicit AtomicSetAccessor(T new_value) : new_value_(new_value) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    atom->store(new_value_, MO);
  }

 private:
  T new_value_;
};

template <typename T> using GetAccessor = AtomicGetAccessor<T, std::memory_order_relaxed>;

template <typename T> using SetAccessor = AtomicSetAccessor<T, std::memory_order_relaxed>;

template <typename T>
using GetVolatileAccessor = AtomicGetAccessor<T, std::memory_order_seq_cst>;

template <typename T>
using SetVolatileAccessor = AtomicSetAccessor<T, std::memory_order_seq_cst>;

template <typename T, std::memory_order MOS, std::memory_order MOF>
class AtomicStrongCompareAndSetAccessor : public Object::Accessor<T> {
 public:
  AtomicStrongCompareAndSetAccessor(T expected_value, T desired_value, JValue* result)
      : expected_value_(expected_value), desired_value_(desired_value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    bool success = atom->compare_exchange_strong(expected_value_, desired_value_, MOS, MOF);
    StoreResult(success ? JNI_TRUE : JNI_FALSE, result_);
  }

 private:
  T expected_value_;
  T desired_value_;
  JValue* result_;
};

template<typename T>
using CompareAndSetAccessor =
    AtomicStrongCompareAndSetAccessor<T, std::memory_order_seq_cst, std::memory_order_seq_cst>;

template <typename T, std::memory_order MOS, std::memory_order MOF>
class AtomicStrongCompareAndExchangeAccessor : public Object::Accessor<T> {
 public:
  AtomicStrongCompareAndExchangeAccessor(T expected_value, T desired_value, JValue* result)
      : expected_value_(expected_value), desired_value_(desired_value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    atom->compare_exchange_strong(expected_value_, desired_value_, MOS, MOF);
    StoreResult(expected_value_, result_);
  }

 private:
  T expected_value_;
  T desired_value_;
  JValue* result_;
};

template <typename T>
using CompareAndExchangeAccessor =
    AtomicStrongCompareAndExchangeAccessor<T, std::memory_order_seq_cst, std::memory_order_seq_cst>;

template <typename T, std::memory_order MOS, std::memory_order MOF>
class AtomicWeakCompareAndSetAccessor : public Object::Accessor<T> {
 public:
  AtomicWeakCompareAndSetAccessor(T expected_value, T desired_value, JValue* result)
      : expected_value_(expected_value), desired_value_(desired_value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    bool success = atom->compare_exchange_weak(expected_value_, desired_value_, MOS, MOF);
    StoreResult(success ? JNI_TRUE : JNI_FALSE, result_);
  }

 private:
  T expected_value_;
  T desired_value_;
  JValue* result_;
};

template <typename T>
using WeakCompareAndSetAccessor =
    AtomicWeakCompareAndSetAccessor<T, std::memory_order_seq_cst, std::memory_order_seq_cst>;

template <typename T, std::memory_order MO>
class AtomicGetAndSetAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndSetAccessor(T new_value, JValue* result) : new_value_(new_value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    T old_value = atom->exchange(new_value_, MO);
    StoreResult(old_value, result_);
  }

 private:
  T new_value_;
  JValue* result_;
};

template <typename T>
using GetAndSetAccessor = AtomicGetAndSetAccessor<T, std::memory_order_seq_cst>;

template <typename T, bool kIsFloat, std::memory_order MO>
class AtomicGetAndAddOperator {
 public:
  static T Apply(T* addr, T addend) {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    return atom->fetch_add(addend, MO);
  }
};

template <typename T, std::memory_order MO>
class AtomicGetAndAddOperator<T, /* kIsFloat */ true, MO> {
 public:
  static T Apply(T* addr, T addend) {
    // c++11 does not have std::atomic<T>::fetch_and_add for floating
    // point types, so we effect one with a compare and swap.
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    T old_value = atom->load(std::memory_order_relaxed);
    T new_value;
    do {
      new_value = old_value + addend;
    } while (!atom->compare_exchange_weak(old_value, new_value, MO, std::memory_order_relaxed));
    return old_value;
  }
};

template <typename T, std::memory_order MO>
class AtomicGetAndAddAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndAddAccessor(T addend, JValue* result) : addend_(addend), result_(result) {}

  void Access(T* addr) override {
    constexpr bool kIsFloatingPoint = std::is_floating_point<T>::value;
    T old_value = AtomicGetAndAddOperator<T, kIsFloatingPoint, MO>::Apply(addr, addend_);
    StoreResult(old_value, result_);
  }

 private:
  T addend_;
  JValue* result_;
};

template <typename T>
using GetAndAddAccessor = AtomicGetAndAddAccessor<T, std::memory_order_seq_cst>;

// Accessor specifically for memory views where the caller can specify
// the byte-ordering. Addition only works outside of the byte-swapped
// memory view because of the direction of carries.
template <typename T, std::memory_order MO>
class AtomicGetAndAddWithByteSwapAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndAddWithByteSwapAccessor(T value, JValue* result) : value_(value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* const atom = reinterpret_cast<std::atomic<T>*>(addr);
    T current_value = atom->load(std::memory_order_relaxed);
    T sum;
    do {
      sum = BSWAP(current_value) + value_;
      // NB current_value is a pass-by-reference argument in the call to
      // atomic<T>::compare_exchange_weak().
    } while (!atom->compare_exchange_weak(current_value,
                                          BSWAP(sum),
                                          MO,
                                          std::memory_order_relaxed));
    StoreResult(BSWAP(current_value), result_);
  }

 private:
  T value_;
  JValue* result_;
};

template <typename T>
using GetAndAddWithByteSwapAccessor =
    AtomicGetAndAddWithByteSwapAccessor<T, std::memory_order_seq_cst>;

template <typename T, std::memory_order MO>
class AtomicGetAndBitwiseOrAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndBitwiseOrAccessor(T value, JValue* result) : value_(value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    T old_value = atom->fetch_or(value_, MO);
    StoreResult(old_value, result_);
  }

 private:
  T value_;
  JValue* result_;
};

template <typename T>
using GetAndBitwiseOrAccessor = AtomicGetAndBitwiseOrAccessor<T, std::memory_order_seq_cst>;

template <typename T, std::memory_order MO>
class AtomicGetAndBitwiseAndAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndBitwiseAndAccessor(T value, JValue* result) : value_(value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    T old_value = atom->fetch_and(value_, MO);
    StoreResult(old_value, result_);
  }

 private:
  T value_;
  JValue* result_;
};

template <typename T>
using GetAndBitwiseAndAccessor =
    AtomicGetAndBitwiseAndAccessor<T, std::memory_order_seq_cst>;

template <typename T, std::memory_order MO>
class AtomicGetAndBitwiseXorAccessor : public Object::Accessor<T> {
 public:
  AtomicGetAndBitwiseXorAccessor(T value, JValue* result) : value_(value), result_(result) {}

  void Access(T* addr) override {
    std::atomic<T>* atom = reinterpret_cast<std::atomic<T>*>(addr);
    T old_value = atom->fetch_xor(value_, MO);
    StoreResult(old_value, result_);
  }

 private:
  T value_;
  JValue* result_;
};

template <typename T>
using GetAndBitwiseXorAccessor = AtomicGetAndBitwiseXorAccessor<T, std::memory_order_seq_cst>;

//
// Unreachable access modes.
//

NO_RETURN void UnreachableAccessMode(const char* access_mode, const char* type_name) {
  LOG(FATAL) << "Unreachable access mode :" << access_mode << " for type " << type_name;
  UNREACHABLE();
}

#define UNREACHABLE_ACCESS_MODE(ACCESS_MODE, TYPE)             \
template<> void ACCESS_MODE ## Accessor<TYPE>::Access(TYPE*) { \
  UnreachableAccessMode(#ACCESS_MODE, #TYPE);                  \
}

// The boolean primitive type is not numeric (boolean == std::uint8_t).
UNREACHABLE_ACCESS_MODE(GetAndAdd, uint8_t)

// The floating point types do not support bitwise operations.
UNREACHABLE_ACCESS_MODE(GetAndBitwiseOr, float)
UNREACHABLE_ACCESS_MODE(GetAndBitwiseAnd, float)
UNREACHABLE_ACCESS_MODE(GetAndBitwiseXor, float)
UNREACHABLE_ACCESS_MODE(GetAndBitwiseOr, double)
UNREACHABLE_ACCESS_MODE(GetAndBitwiseAnd, double)
UNREACHABLE_ACCESS_MODE(GetAndBitwiseXor, double)

// A helper class for object field accesses for floats and
// doubles. The object interface deals with Field32 and Field64. The
// former is used for both integers and floats, the latter for longs
// and doubles. This class provides the necessary coercion.
template <typename T, typename U>
class TypeAdaptorAccessor : public Object::Accessor<T> {
 public:
  explicit TypeAdaptorAccessor(Object::Accessor<U>* inner_accessor)
      : inner_accessor_(inner_accessor) {}

  void Access(T* addr) override {
    static_assert(sizeof(T) == sizeof(U), "bad conversion");
    inner_accessor_->Access(reinterpret_cast<U*>(addr));
  }

 private:
  Object::Accessor<U>* inner_accessor_;
};

template <typename T>
class FieldAccessViaAccessor {
 public:
  using Accessor = Object::Accessor<T>;

  // Apply an Accessor to get a field in an object.
  static void Get(ObjPtr<Object> obj,
                  MemberOffset field_offset,
                  Accessor* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    obj->GetPrimitiveFieldViaAccessor(field_offset, accessor);
  }

  // Apply an Accessor to update a field in an object.
  static void Update(ObjPtr<Object> obj,
                     MemberOffset field_offset,
                     Accessor* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
};

template <>
inline void FieldAccessViaAccessor<float>::Get(ObjPtr<Object> obj,
                                               MemberOffset field_offset,
                                               Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  TypeAdaptorAccessor<int32_t, float> float_to_int_accessor(accessor);
  obj->GetPrimitiveFieldViaAccessor(field_offset, &float_to_int_accessor);
}

template <>
inline void FieldAccessViaAccessor<double>::Get(ObjPtr<Object> obj,
                                                MemberOffset field_offset,
                                                Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  TypeAdaptorAccessor<int64_t, double> double_to_int_accessor(accessor);
  obj->GetPrimitiveFieldViaAccessor(field_offset, &double_to_int_accessor);
}

template <>
void FieldAccessViaAccessor<uint8_t>::Update(ObjPtr<Object> obj,
                                             MemberOffset field_offset,
                                             Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateFieldBooleanViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateFieldBooleanViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<int8_t>::Update(ObjPtr<Object> obj,
                                            MemberOffset field_offset,
                                            Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateFieldByteViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateFieldByteViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<uint16_t>::Update(ObjPtr<Object> obj,
                                              MemberOffset field_offset,
                                              Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateFieldCharViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateFieldCharViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<int16_t>::Update(ObjPtr<Object> obj,
                                              MemberOffset field_offset,
                                              Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateFieldShortViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateFieldShortViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<int32_t>::Update(ObjPtr<Object> obj,
                                             MemberOffset field_offset,
                                             Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateField32ViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateField32ViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<int64_t>::Update(ObjPtr<Object> obj,
                                             MemberOffset field_offset,
                                             Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateField64ViaAccessor<kTransactionActive>(field_offset, accessor);
  } else {
    obj->UpdateField64ViaAccessor<kTransactionInactive>(field_offset, accessor);
  }
}

template <>
void FieldAccessViaAccessor<float>::Update(ObjPtr<Object> obj,
                                           MemberOffset field_offset,
                                           Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  TypeAdaptorAccessor<int32_t, float> float_to_int_accessor(accessor);
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateField32ViaAccessor<kTransactionActive>(field_offset, &float_to_int_accessor);
  } else {
    obj->UpdateField32ViaAccessor<kTransactionInactive>(field_offset, &float_to_int_accessor);
  }
}

template <>
void FieldAccessViaAccessor<double>::Update(ObjPtr<Object> obj,
                                            MemberOffset field_offset,
                                            Accessor* accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  TypeAdaptorAccessor<int64_t, double> double_to_int_accessor(accessor);
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->UpdateField64ViaAccessor<kTransactionActive>(field_offset, &double_to_int_accessor);
  } else {
    obj->UpdateField64ViaAccessor<kTransactionInactive>(field_offset, &double_to_int_accessor);
  }
}

// Helper class that gets values from a shadow frame with appropriate type coercion.
template <typename T>
class ValueGetter {
 public:
  static T Get(ShadowFrameGetter* getter) REQUIRES_SHARED(Locks::mutator_lock_) {
    static_assert(sizeof(T) <= sizeof(uint32_t), "Bad size");
    uint32_t raw_value = getter->Get();
    return static_cast<T>(raw_value);
  }
};

template <>
int64_t ValueGetter<int64_t>::Get(ShadowFrameGetter* getter) {
  return getter->GetLong();
}

template <>
float ValueGetter<float>::Get(ShadowFrameGetter* getter) {
  uint32_t raw_value = getter->Get();
  return *reinterpret_cast<float*>(&raw_value);
}

template <>
double ValueGetter<double>::Get(ShadowFrameGetter* getter) {
  int64_t raw_value = getter->GetLong();
  return *reinterpret_cast<double*>(&raw_value);
}

template <>
ObjPtr<Object> ValueGetter<ObjPtr<Object>>::Get(ShadowFrameGetter* getter) {
  return getter->GetReference();
}

// Class for accessing fields of Object instances
template <typename T>
class FieldAccessor {
 public:
  static bool Dispatch(VarHandle::AccessMode access_mode,
                       ObjPtr<Object> obj,
                       MemberOffset field_offset,
                       ShadowFrameGetter* getter,
                       JValue* result)
      REQUIRES_SHARED(Locks::mutator_lock_);
};

// Dispatch implementation for primitive fields.
template <typename T>
bool FieldAccessor<T>::Dispatch(VarHandle::AccessMode access_mode,
                                ObjPtr<Object> obj,
                                MemberOffset field_offset,
                                ShadowFrameGetter* getter,
                                JValue* result) {
  switch (access_mode) {
    case VarHandle::AccessMode::kGet: {
      GetAccessor<T> accessor(result);
      FieldAccessViaAccessor<T>::Get(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kSet: {
      T new_value = ValueGetter<T>::Get(getter);
      SetAccessor<T> accessor(new_value);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAcquire:
    case VarHandle::AccessMode::kGetOpaque:
    case VarHandle::AccessMode::kGetVolatile: {
      GetVolatileAccessor<T> accessor(result);
      FieldAccessViaAccessor<T>::Get(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kSetOpaque:
    case VarHandle::AccessMode::kSetRelease:
    case VarHandle::AccessMode::kSetVolatile: {
      T new_value = ValueGetter<T>::Get(getter);
      SetVolatileAccessor<T> accessor(new_value);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kCompareAndSet: {
      T expected_value = ValueGetter<T>::Get(getter);
      T desired_value = ValueGetter<T>::Get(getter);
      CompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kCompareAndExchange:
    case VarHandle::AccessMode::kCompareAndExchangeAcquire:
    case VarHandle::AccessMode::kCompareAndExchangeRelease: {
      T expected_value = ValueGetter<T>::Get(getter);
      T desired_value = ValueGetter<T>::Get(getter);
      CompareAndExchangeAccessor<T> accessor(expected_value, desired_value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kWeakCompareAndSet:
    case VarHandle::AccessMode::kWeakCompareAndSetAcquire:
    case VarHandle::AccessMode::kWeakCompareAndSetPlain:
    case VarHandle::AccessMode::kWeakCompareAndSetRelease: {
      T expected_value = ValueGetter<T>::Get(getter);
      T desired_value = ValueGetter<T>::Get(getter);
      WeakCompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAndSet:
    case VarHandle::AccessMode::kGetAndSetAcquire:
    case VarHandle::AccessMode::kGetAndSetRelease: {
      T new_value = ValueGetter<T>::Get(getter);
      GetAndSetAccessor<T> accessor(new_value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAndAdd:
    case VarHandle::AccessMode::kGetAndAddAcquire:
    case VarHandle::AccessMode::kGetAndAddRelease: {
      T value = ValueGetter<T>::Get(getter);
      GetAndAddAccessor<T> accessor(value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAndBitwiseOr:
    case VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseOrRelease: {
      T value = ValueGetter<T>::Get(getter);
      GetAndBitwiseOrAccessor<T> accessor(value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAndBitwiseAnd:
    case VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseAndRelease: {
      T value = ValueGetter<T>::Get(getter);
      GetAndBitwiseAndAccessor<T> accessor(value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
    case VarHandle::AccessMode::kGetAndBitwiseXor:
    case VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseXorRelease: {
      T value = ValueGetter<T>::Get(getter);
      GetAndBitwiseXorAccessor<T> accessor(value, result);
      FieldAccessViaAccessor<T>::Update(obj, field_offset, &accessor);
      break;
    }
  }
  return true;
}

// Dispatch implementation for reference fields.
template <>
bool FieldAccessor<ObjPtr<Object>>::Dispatch(VarHandle::AccessMode access_mode,
                                             ObjPtr<Object> obj,
                                             MemberOffset field_offset,
                                             ShadowFrameGetter* getter,
                                             JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // To keep things simple, use the minimum strongest existing
  // field accessor for Object fields. This may be the most
  // straightforward strategy in general for the interpreter.
  switch (access_mode) {
    case VarHandle::AccessMode::kGet: {
      StoreResult(obj->GetFieldObject<Object>(field_offset), result);
      break;
    }
    case VarHandle::AccessMode::kSet: {
      ObjPtr<Object> new_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      if (Runtime::Current()->IsActiveTransaction()) {
        obj->SetFieldObject<kTransactionActive>(field_offset, new_value);
      } else {
        obj->SetFieldObject<kTransactionInactive>(field_offset, new_value);
      }
      break;
    }
    case VarHandle::AccessMode::kGetAcquire:
    case VarHandle::AccessMode::kGetOpaque:
    case VarHandle::AccessMode::kGetVolatile: {
      StoreResult(obj->GetFieldObjectVolatile<Object>(field_offset), result);
      break;
    }
    case VarHandle::AccessMode::kSetOpaque:
    case VarHandle::AccessMode::kSetRelease:
    case VarHandle::AccessMode::kSetVolatile: {
      ObjPtr<Object> new_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      if (Runtime::Current()->IsActiveTransaction()) {
        obj->SetFieldObjectVolatile<kTransactionActive>(field_offset, new_value);
      } else {
        obj->SetFieldObjectVolatile<kTransactionInactive>(field_offset, new_value);
      }
      break;
    }
    case VarHandle::AccessMode::kCompareAndSet: {
      ReadBarrierForVarHandleAccess(obj, field_offset);
      ObjPtr<Object> expected_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      ObjPtr<Object> desired_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      bool cas_result;
      if (Runtime::Current()->IsActiveTransaction()) {
        cas_result = obj->CasFieldObject<kTransactionActive>(field_offset,
                                                             expected_value,
                                                             desired_value,
                                                             CASMode::kStrong,
                                                             std::memory_order_seq_cst);
      } else {
        cas_result = obj->CasFieldObject<kTransactionInactive>(field_offset,
                                                               expected_value,
                                                               desired_value,
                                                               CASMode::kStrong,
                                                               std::memory_order_seq_cst);
      }
      StoreResult(static_cast<uint8_t>(cas_result), result);
      break;
    }
    case VarHandle::AccessMode::kWeakCompareAndSet:
    case VarHandle::AccessMode::kWeakCompareAndSetAcquire:
    case VarHandle::AccessMode::kWeakCompareAndSetPlain:
    case VarHandle::AccessMode::kWeakCompareAndSetRelease: {
      ReadBarrierForVarHandleAccess(obj, field_offset);
      ObjPtr<Object> expected_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      ObjPtr<Object> desired_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      bool cas_result;
      if (Runtime::Current()->IsActiveTransaction()) {
        cas_result = obj->CasFieldObject<kTransactionActive>(field_offset,
                                                             expected_value,
                                                             desired_value,
                                                             CASMode::kWeak,
                                                             std::memory_order_seq_cst);
      } else {
        cas_result = obj->CasFieldObject<kTransactionInactive>(
            field_offset,
            expected_value,
            desired_value,
            CASMode::kWeak,
            std::memory_order_seq_cst);
      }
      StoreResult(static_cast<uint8_t>(cas_result), result);
      break;
    }
    case VarHandle::AccessMode::kCompareAndExchange:
    case VarHandle::AccessMode::kCompareAndExchangeAcquire:
    case VarHandle::AccessMode::kCompareAndExchangeRelease: {
      ReadBarrierForVarHandleAccess(obj, field_offset);
      ObjPtr<Object> expected_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      ObjPtr<Object> desired_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      ObjPtr<Object> witness_value;
      if (Runtime::Current()->IsActiveTransaction()) {
        witness_value = obj->CompareAndExchangeFieldObject<kTransactionActive>(field_offset,
                                                                               expected_value,
                                                                               desired_value);
      } else {
        witness_value = obj->CompareAndExchangeFieldObject<kTransactionInactive>(field_offset,
                                                                                 expected_value,
                                                                                 desired_value);
      }
      StoreResult(witness_value, result);
      break;
    }
    case VarHandle::AccessMode::kGetAndSet:
    case VarHandle::AccessMode::kGetAndSetAcquire:
    case VarHandle::AccessMode::kGetAndSetRelease: {
      ReadBarrierForVarHandleAccess(obj, field_offset);
      ObjPtr<Object> new_value = ValueGetter<ObjPtr<Object>>::Get(getter);
      ObjPtr<Object> old_value;
      if (Runtime::Current()->IsActiveTransaction()) {
        old_value = obj->ExchangeFieldObject<kTransactionActive>(field_offset, new_value);
      } else {
        old_value = obj->ExchangeFieldObject<kTransactionInactive>(field_offset, new_value);
      }
      StoreResult(old_value, result);
      break;
    }
    case VarHandle::AccessMode::kGetAndAdd:
    case VarHandle::AccessMode::kGetAndAddAcquire:
    case VarHandle::AccessMode::kGetAndAddRelease:
    case VarHandle::AccessMode::kGetAndBitwiseOr:
    case VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseOrRelease:
    case VarHandle::AccessMode::kGetAndBitwiseAnd:
    case VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseAndRelease:
    case VarHandle::AccessMode::kGetAndBitwiseXor:
    case VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
    case VarHandle::AccessMode::kGetAndBitwiseXorRelease: {
      size_t index = static_cast<size_t>(access_mode);
      const char* access_mode_name = kAccessorToAccessMode[index].method_name;
      UnreachableAccessMode(access_mode_name, "Object");
    }
  }
  return true;
}

// Class for accessing primitive array elements.
template <typename T>
class PrimitiveArrayElementAccessor {
 public:
  static T* GetElementAddress(ObjPtr<Array> target_array, int target_element)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    auto primitive_array = ObjPtr<PrimitiveArray<T>>::DownCast(target_array);
    DCHECK(primitive_array->CheckIsValidIndex(target_element));
    return &primitive_array->GetData()[target_element];
  }

  static bool Dispatch(VarHandle::AccessMode access_mode,
                       ObjPtr<Array> target_array,
                       int target_element,
                       ShadowFrameGetter* getter,
                       JValue* result)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    T* element_address = GetElementAddress(target_array, target_element);
    switch (access_mode) {
      case VarHandle::AccessMode::kGet: {
        GetAccessor<T> accessor(result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kSet: {
        T new_value = ValueGetter<T>::Get(getter);
        SetAccessor<T> accessor(new_value);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAcquire:
      case VarHandle::AccessMode::kGetOpaque:
      case VarHandle::AccessMode::kGetVolatile: {
        GetVolatileAccessor<T> accessor(result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kSetOpaque:
      case VarHandle::AccessMode::kSetRelease:
      case VarHandle::AccessMode::kSetVolatile: {
        T new_value = ValueGetter<T>::Get(getter);
        SetVolatileAccessor<T> accessor(new_value);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kCompareAndSet: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        CompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kCompareAndExchange:
      case VarHandle::AccessMode::kCompareAndExchangeAcquire:
      case VarHandle::AccessMode::kCompareAndExchangeRelease: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        CompareAndExchangeAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kWeakCompareAndSet:
      case VarHandle::AccessMode::kWeakCompareAndSetAcquire:
      case VarHandle::AccessMode::kWeakCompareAndSetPlain:
      case VarHandle::AccessMode::kWeakCompareAndSetRelease: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        WeakCompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndSet:
      case VarHandle::AccessMode::kGetAndSetAcquire:
      case VarHandle::AccessMode::kGetAndSetRelease: {
        T new_value = ValueGetter<T>::Get(getter);
        GetAndSetAccessor<T> accessor(new_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndAdd:
      case VarHandle::AccessMode::kGetAndAddAcquire:
      case VarHandle::AccessMode::kGetAndAddRelease: {
        T value = ValueGetter<T>::Get(getter);
        GetAndAddAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseOr:
      case VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseOrRelease: {
        T value = ValueGetter<T>::Get(getter);
        GetAndBitwiseOrAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseAnd:
      case VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseAndRelease: {
        T value = ValueGetter<T>::Get(getter);
        GetAndBitwiseAndAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseXor:
      case VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseXorRelease: {
        T value = ValueGetter<T>::Get(getter);
        GetAndBitwiseXorAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        break;
      }
    }
    return true;
  }
};

// Class for accessing primitive array elements.
template <typename T>
class ByteArrayViewAccessor {
 public:
  static inline bool IsAccessAligned(int8_t* data, int data_index) {
    static_assert(IsPowerOfTwo(sizeof(T)), "unexpected size");
    static_assert(std::is_arithmetic<T>::value, "unexpected type");
    uintptr_t alignment_mask = sizeof(T) - 1;
    uintptr_t address = reinterpret_cast<uintptr_t>(data + data_index);
    return (address & alignment_mask) == 0;
  }

  static inline void MaybeByteSwap(bool byte_swap, T* const value) {
    if (byte_swap) {
      *value = BSWAP(*value);
    }
  }

  static bool Dispatch(const VarHandle::AccessMode access_mode,
                       int8_t* const data,
                       const int data_index,
                       const bool byte_swap,
                       ShadowFrameGetter* const getter,
                       JValue* const result)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const bool is_aligned = IsAccessAligned(data, data_index);
    if (!is_aligned) {
      switch (access_mode) {
        case VarHandle::AccessMode::kGet: {
          T value;
          memcpy(&value, data + data_index, sizeof(T));
          MaybeByteSwap(byte_swap, &value);
          StoreResult(value, result);
          return true;
        }
        case VarHandle::AccessMode::kSet: {
          T new_value = ValueGetter<T>::Get(getter);
          MaybeByteSwap(byte_swap, &new_value);
          memcpy(data + data_index, &new_value, sizeof(T));
          return true;
        }
        default:
          // No other access modes support unaligned access.
          ThrowIllegalStateException("Unaligned access not supported");
          return false;
      }
    }

    T* const element_address = reinterpret_cast<T*>(data + data_index);
    CHECK(IsAccessAligned(reinterpret_cast<int8_t*>(element_address), 0));
    switch (access_mode) {
      case VarHandle::AccessMode::kGet: {
        GetAccessor<T> accessor(result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kSet: {
        T new_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &new_value);
        SetAccessor<T> accessor(new_value);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAcquire:
      case VarHandle::AccessMode::kGetOpaque:
      case VarHandle::AccessMode::kGetVolatile: {
        GetVolatileAccessor<T> accessor(result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kSetOpaque:
      case VarHandle::AccessMode::kSetRelease:
      case VarHandle::AccessMode::kSetVolatile: {
        T new_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &new_value);
        SetVolatileAccessor<T> accessor(new_value);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kCompareAndSet: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &expected_value);
        MaybeByteSwap(byte_swap, &desired_value);
        CompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kCompareAndExchange:
      case VarHandle::AccessMode::kCompareAndExchangeAcquire:
      case VarHandle::AccessMode::kCompareAndExchangeRelease: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &expected_value);
        MaybeByteSwap(byte_swap, &desired_value);
        CompareAndExchangeAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kWeakCompareAndSet:
      case VarHandle::AccessMode::kWeakCompareAndSetAcquire:
      case VarHandle::AccessMode::kWeakCompareAndSetPlain:
      case VarHandle::AccessMode::kWeakCompareAndSetRelease: {
        T expected_value = ValueGetter<T>::Get(getter);
        T desired_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &expected_value);
        MaybeByteSwap(byte_swap, &desired_value);
        WeakCompareAndSetAccessor<T> accessor(expected_value, desired_value, result);
        accessor.Access(element_address);
        break;
      }
      case VarHandle::AccessMode::kGetAndSet:
      case VarHandle::AccessMode::kGetAndSetAcquire:
      case VarHandle::AccessMode::kGetAndSetRelease: {
        T new_value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &new_value);
        GetAndSetAccessor<T> accessor(new_value, result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kGetAndAdd:
      case VarHandle::AccessMode::kGetAndAddAcquire:
      case VarHandle::AccessMode::kGetAndAddRelease: {
        T value = ValueGetter<T>::Get(getter);
        if (byte_swap) {
          GetAndAddWithByteSwapAccessor<T> accessor(value, result);
          accessor.Access(element_address);
        } else {
          GetAndAddAccessor<T> accessor(value, result);
          accessor.Access(element_address);
        }
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseOr:
      case VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseOrRelease: {
        T value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &value);
        GetAndBitwiseOrAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseAnd:
      case VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseAndRelease: {
        T value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &value);
        GetAndBitwiseAndAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
      case VarHandle::AccessMode::kGetAndBitwiseXor:
      case VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
      case VarHandle::AccessMode::kGetAndBitwiseXorRelease: {
        T value = ValueGetter<T>::Get(getter);
        MaybeByteSwap(byte_swap, &value);
        GetAndBitwiseXorAccessor<T> accessor(value, result);
        accessor.Access(element_address);
        JValueByteSwapper<T>::MaybeByteSwap(byte_swap, result);
        break;
      }
    }
    return true;
  }
};

}  // namespace

ObjPtr<Class> VarHandle::GetVarType() {
  return GetFieldObject<Class>(VarTypeOffset());
}

ObjPtr<Class> VarHandle::GetCoordinateType0() {
  return GetFieldObject<Class>(CoordinateType0Offset());
}

ObjPtr<Class> VarHandle::GetCoordinateType1() {
  return GetFieldObject<Class>(CoordinateType1Offset());
}

int32_t VarHandle::GetAccessModesBitMask() {
  return GetField32(AccessModesBitMaskOffset());
}

template <typename MethodTypeType>
VarHandle::MatchKind VarHandle::GetMethodTypeMatchForAccessModeImpl(
    AccessMode access_mode, ObjPtr<VarHandle> var_handle, MethodTypeType method_type) {
  MatchKind match = MatchKind::kExact;

  ObjPtr<Class> var_type = var_handle->GetVarType();
  ObjPtr<Class> mt_rtype = MethodType::GetRType(method_type);
  ObjPtr<Class> void_type = WellKnownClasses::ToClass(WellKnownClasses::java_lang_Void);
  AccessModeTemplate access_mode_template = GetAccessModeTemplate(access_mode);

  // Check return type first.
  ObjPtr<Class> vh_rtype = GetReturnType(access_mode_template, var_type);
  if (mt_rtype->GetPrimitiveType() != Primitive::Type::kPrimVoid &&
      !mt_rtype->IsAssignableFrom(vh_rtype)) {
    // Call-site is an expression (expects a return value) and the value returned by the accessor
    // is not assignable to the expected return type.
    if (!IsReturnTypeConvertible(vh_rtype, mt_rtype)) {
      return MatchKind::kNone;
    }
    match = MatchKind::kWithConversions;
  }

  // Check the number of parameters matches.
  ObjPtr<Class> vh_ptypes[kMaxAccessorParameters];
  const int32_t vh_ptypes_count = BuildParameterArray(vh_ptypes,
                                                      access_mode_template,
                                                      var_type,
                                                      var_handle->GetCoordinateType0(),
                                                      var_handle->GetCoordinateType1());
  auto mt_ptypes = MethodType::GetPTypes(method_type);
  if (vh_ptypes_count != mt_ptypes.GetLength()) {
    return MatchKind::kNone;
  }

  // Check the parameter types are compatible.
  for (int32_t i = 0; i < vh_ptypes_count; ++i) {
    if (vh_ptypes[i]->IsAssignableFrom(mt_ptypes.Get(i))) {
      continue;
    }
    if (mt_ptypes.Get(i) == void_type && !vh_ptypes[i]->IsPrimitive()) {
      // The expected parameter is a reference and the parameter type from the call site is j.l.Void
      // which means the value is null. It is always valid for a reference parameter to be null.
      continue;
    }
    if (!IsParameterTypeConvertible(mt_ptypes.Get(i), vh_ptypes[i])) {
      return MatchKind::kNone;
    }
    match = MatchKind::kWithConversions;
  }
  return match;
}

VarHandle::MatchKind VarHandle::GetMethodTypeMatchForAccessMode(
    AccessMode access_mode, ObjPtr<MethodType> method_type) {
  return GetMethodTypeMatchForAccessModeImpl(access_mode, this, method_type);
}

VarHandle::MatchKind VarHandle::GetMethodTypeMatchForAccessMode(
    AccessMode access_mode, Handle<MethodType> method_type) {
  return GetMethodTypeMatchForAccessMode(access_mode, method_type.Get());
}

VarHandle::MatchKind VarHandle::GetMethodTypeMatchForAccessMode(
    AccessMode access_mode, RawMethodType method_type) {
  return GetMethodTypeMatchForAccessModeImpl(access_mode, this, method_type);
}

ObjPtr<MethodType> VarHandle::GetMethodTypeForAccessMode(Thread* self, AccessMode access_mode) {
  VariableSizedHandleScope method_type_hs(self);
  RawMethodType method_type(&method_type_hs);
  GetMethodTypeForAccessMode(access_mode, method_type);
  return MethodType::Create(self, method_type);
}

void VarHandle::GetMethodTypeForAccessMode(AccessMode access_mode,
                                           /*out*/ RawMethodType method_type) {
  DCHECK(!method_type.IsValid());
  AccessModeTemplate access_mode_template = GetAccessModeTemplate(access_mode);

  // Store return type in `method_type`.
  method_type.SetRType(GetReturnType(access_mode_template, GetVarType()));

  // Store parameter types in `method_type`.
  ObjPtr<Class> ptypes_array[kMaxAccessorParameters];
  int32_t ptypes_count = BuildParameterArray(ptypes_array,
                                             access_mode_template,
                                             GetVarType(),
                                             GetCoordinateType0(),
                                             GetCoordinateType1());
  for (int32_t i = 0; i < ptypes_count; ++i) {
    method_type.AddPType(ptypes_array[i]);
  }
}

std::string VarHandle::PrettyDescriptorForAccessMode(AccessMode access_mode) {
  // Effect MethodType::PrettyDescriptor() without first creating a method type first.
  std::ostringstream oss;
  oss << '(';

  AccessModeTemplate access_mode_template = GetAccessModeTemplate(access_mode);
  ObjPtr<Class> var_type = GetVarType();
  ObjPtr<Class> ctypes[2] = { GetCoordinateType0(), GetCoordinateType1() };
  const int32_t ptypes_count = GetNumberOfParameters(access_mode_template, ctypes[0], ctypes[1]);
  int32_t ptypes_done = 0;
  for (ObjPtr<Class> ctype : ctypes) {
    if (!ctype.IsNull()) {
      if (ptypes_done != 0) {
        oss << ", ";
      }
      oss << ctype->PrettyDescriptor();;
      ptypes_done++;
    }
  }
  while (ptypes_done != ptypes_count) {
    if (ptypes_done != 0) {
      oss << ", ";
    }
    oss << var_type->PrettyDescriptor();
    ptypes_done++;
  }
  ObjPtr<Class> rtype = GetReturnType(access_mode_template, var_type);
  oss << ')' << rtype->PrettyDescriptor();
  return oss.str();
}

bool VarHandle::Access(AccessMode access_mode,
                       ShadowFrame* shadow_frame,
                       const InstructionOperands* const operands,
                       JValue* result) {
  ObjPtr<ObjectArray<Class>> class_roots = Runtime::Current()->GetClassLinker()->GetClassRoots();
  ObjPtr<Class> klass = GetClass();
  if (klass == GetClassRoot<FieldVarHandle>(class_roots)) {
    auto vh = ObjPtr<FieldVarHandle>::DownCast(this);
    return vh->Access(access_mode, shadow_frame, operands, result);
  } else if (klass == GetClassRoot<StaticFieldVarHandle>(class_roots)) {
    auto vh = ObjPtr<StaticFieldVarHandle>::DownCast(this);
    return vh->Access(access_mode, shadow_frame, operands, result);
  } else if (klass == GetClassRoot<ArrayElementVarHandle>(class_roots)) {
    auto vh = ObjPtr<ArrayElementVarHandle>::DownCast(this);
    return vh->Access(access_mode, shadow_frame, operands, result);
  } else if (klass == GetClassRoot<ByteArrayViewVarHandle>(class_roots)) {
    auto vh = ObjPtr<ByteArrayViewVarHandle>::DownCast(this);
    return vh->Access(access_mode, shadow_frame, operands, result);
  } else if (klass == GetClassRoot<ByteBufferViewVarHandle>(class_roots)) {
    auto vh = ObjPtr<ByteBufferViewVarHandle>::DownCast(this);
    return vh->Access(access_mode, shadow_frame, operands, result);
  } else {
    LOG(FATAL) << "Unknown varhandle kind";
    UNREACHABLE();
  }
}

const char* VarHandle::GetReturnTypeDescriptor(const char* accessor_name) {
  AccessMode access_mode;
  if (!GetAccessModeByMethodName(accessor_name, &access_mode)) {
    return nullptr;
  }
  AccessModeTemplate access_mode_template = GetAccessModeTemplate(access_mode);
  switch (access_mode_template) {
    case AccessModeTemplate::kGet:
    case AccessModeTemplate::kCompareAndExchange:
    case AccessModeTemplate::kGetAndUpdate:
      return "Ljava/lang/Object;";
    case AccessModeTemplate::kCompareAndSet:
      return "Z";
    case AccessModeTemplate::kSet:
      return "V";
  }
}

VarHandle::AccessMode VarHandle::GetAccessModeByIntrinsic(Intrinsics intrinsic) {
#define VAR_HANDLE_ACCESS_MODE(V)               \
    V(CompareAndExchange)                       \
    V(CompareAndExchangeAcquire)                \
    V(CompareAndExchangeRelease)                \
    V(CompareAndSet)                            \
    V(Get)                                      \
    V(GetAcquire)                               \
    V(GetAndAdd)                                \
    V(GetAndAddAcquire)                         \
    V(GetAndAddRelease)                         \
    V(GetAndBitwiseAnd)                         \
    V(GetAndBitwiseAndAcquire)                  \
    V(GetAndBitwiseAndRelease)                  \
    V(GetAndBitwiseOr)                          \
    V(GetAndBitwiseOrAcquire)                   \
    V(GetAndBitwiseOrRelease)                   \
    V(GetAndBitwiseXor)                         \
    V(GetAndBitwiseXorAcquire)                  \
    V(GetAndBitwiseXorRelease)                  \
    V(GetAndSet)                                \
    V(GetAndSetAcquire)                         \
    V(GetAndSetRelease)                         \
    V(GetOpaque)                                \
    V(GetVolatile)                              \
    V(Set)                                      \
    V(SetOpaque)                                \
    V(SetRelease)                               \
    V(SetVolatile)                              \
    V(WeakCompareAndSet)                        \
    V(WeakCompareAndSetAcquire)                 \
    V(WeakCompareAndSetPlain)                   \
    V(WeakCompareAndSetRelease)
  switch (intrinsic) {
#define INTRINSIC_CASE(Name)                    \
    case Intrinsics::kVarHandle ## Name:        \
      return VarHandle::AccessMode::k ## Name;
    VAR_HANDLE_ACCESS_MODE(INTRINSIC_CASE)
#undef INTRINSIC_CASE
#undef VAR_HANDLE_ACCESS_MODE
    default:
      break;
  }
  LOG(FATAL) << "Unknown VarHandle instrinsic: " << static_cast<int>(intrinsic);
  UNREACHABLE();
}

bool VarHandle::GetAccessModeByMethodName(const char* method_name, AccessMode* access_mode) {
  if (method_name == nullptr) {
    return false;
  }
  const auto kUnusedAccessMode = VarHandle::AccessMode::kGet;  // arbitrary value.
  VarHandleAccessorToAccessModeEntry target = { method_name, kUnusedAccessMode };
  auto last = std::cend(kAccessorToAccessMode);
  auto it = std::lower_bound(std::cbegin(kAccessorToAccessMode),
                             last,
                             target,
                             VarHandleAccessorToAccessModeEntry::CompareName);
  if (it == last || strcmp(it->method_name, method_name) != 0) {
    return false;
  }
  *access_mode = it->access_mode;
  return true;
}

// Look up the AccessModeTemplate for a given VarHandle
// AccessMode. This simplifies finding the correct signature for a
// VarHandle accessor method.
VarHandle::AccessModeTemplate VarHandle::GetAccessModeTemplate(VarHandle::AccessMode access_mode) {
  switch (access_mode) {
    case VarHandle::AccessMode::kGet:
      return AccessModeTemplate::kGet;
    case VarHandle::AccessMode::kSet:
      return AccessModeTemplate::kSet;
    case VarHandle::AccessMode::kGetVolatile:
      return AccessModeTemplate::kGet;
    case VarHandle::AccessMode::kSetVolatile:
      return AccessModeTemplate::kSet;
    case VarHandle::AccessMode::kGetAcquire:
      return AccessModeTemplate::kGet;
    case VarHandle::AccessMode::kSetRelease:
      return AccessModeTemplate::kSet;
    case VarHandle::AccessMode::kGetOpaque:
      return AccessModeTemplate::kGet;
    case VarHandle::AccessMode::kSetOpaque:
      return AccessModeTemplate::kSet;
    case VarHandle::AccessMode::kCompareAndSet:
      return AccessModeTemplate::kCompareAndSet;
    case VarHandle::AccessMode::kCompareAndExchange:
      return AccessModeTemplate::kCompareAndExchange;
    case VarHandle::AccessMode::kCompareAndExchangeAcquire:
      return AccessModeTemplate::kCompareAndExchange;
    case VarHandle::AccessMode::kCompareAndExchangeRelease:
      return AccessModeTemplate::kCompareAndExchange;
    case VarHandle::AccessMode::kWeakCompareAndSetPlain:
      return AccessModeTemplate::kCompareAndSet;
    case VarHandle::AccessMode::kWeakCompareAndSet:
      return AccessModeTemplate::kCompareAndSet;
    case VarHandle::AccessMode::kWeakCompareAndSetAcquire:
      return AccessModeTemplate::kCompareAndSet;
    case VarHandle::AccessMode::kWeakCompareAndSetRelease:
      return AccessModeTemplate::kCompareAndSet;
    case VarHandle::AccessMode::kGetAndSet:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndSetAcquire:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndSetRelease:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndAdd:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndAddAcquire:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndAddRelease:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseOr:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseOrRelease:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseOrAcquire:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseAnd:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseAndRelease:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseAndAcquire:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseXor:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseXorRelease:
      return AccessModeTemplate::kGetAndUpdate;
    case VarHandle::AccessMode::kGetAndBitwiseXorAcquire:
      return AccessModeTemplate::kGetAndUpdate;
  }
}

VarHandle::AccessModeTemplate VarHandle::GetAccessModeTemplateByIntrinsic(Intrinsics ordinal) {
  AccessMode access_mode = GetAccessModeByIntrinsic(ordinal);
  return GetAccessModeTemplate(access_mode);
}

int32_t VarHandle::GetNumberOfVarTypeParameters(AccessModeTemplate access_mode_template) {
  switch (access_mode_template) {
    case AccessModeTemplate::kGet:
      return 0;
    case AccessModeTemplate::kSet:
    case AccessModeTemplate::kGetAndUpdate:
      return 1;
    case AccessModeTemplate::kCompareAndSet:
    case AccessModeTemplate::kCompareAndExchange:
      return 2;
  }
}

bool FieldVarHandle::Access(AccessMode access_mode,
                            ShadowFrame* shadow_frame,
                            const InstructionOperands* const operands,
                            JValue* result) {
  ShadowFrameGetter getter(*shadow_frame, operands);
  ArtField* field = GetArtField();
  ObjPtr<Object> obj;
  if (field->IsStatic()) {
    DCHECK_LE(operands->GetNumberOfOperands(),
              2u * (Primitive::Is64BitType(GetVarType()->GetPrimitiveType()) ? 2u : 1u));
    obj = field->GetDeclaringClass();
  } else {
    DCHECK_GE(operands->GetNumberOfOperands(), 1u);
    DCHECK_LE(operands->GetNumberOfOperands(),
              1u + 2u * (Primitive::Is64BitType(GetVarType()->GetPrimitiveType()) ? 2u : 1u));
    obj = getter.GetReference();
    if (obj.IsNull()) {
      ThrowNullPointerExceptionForCoordinate();
      return false;
    }
  }
  DCHECK(!obj.IsNull());

  const MemberOffset offset = field->GetOffset();
  const Primitive::Type primitive_type = GetVarType()->GetPrimitiveType();
  switch (primitive_type) {
    case Primitive::Type::kPrimNot:
      return FieldAccessor<ObjPtr<Object>>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimBoolean:
      return FieldAccessor<uint8_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimByte:
      return FieldAccessor<int8_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimChar:
      return FieldAccessor<uint16_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimShort:
      return FieldAccessor<int16_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimInt:
      return FieldAccessor<int32_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimFloat:
      return FieldAccessor<float>::Dispatch(access_mode,  obj, offset, &getter, result);
    case Primitive::kPrimLong:
      return FieldAccessor<int64_t>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimDouble:
      return FieldAccessor<double>::Dispatch(access_mode, obj, offset, &getter, result);
    case Primitive::kPrimVoid:
      break;
  }
  LOG(FATAL) << "Unreachable: Unexpected primitive " << primitive_type;
  UNREACHABLE();
}

bool ArrayElementVarHandle::CheckArrayStore(AccessMode access_mode,
                                            ShadowFrameGetter getter,
                                            ObjPtr<ObjectArray<Object>> array) {
  // This method checks the element being inserted into the array is correctly assignable.
  // NB This method assumes it is called from `ArrayElementVarHandle::Access()` and `getter`
  // has already consumed the array and index arguments.
  ObjPtr<Object> new_element;
  switch (GetAccessModeTemplate(access_mode)) {
    case AccessModeTemplate::kGet:
      return true;  // Not a store.
    case AccessModeTemplate::kCompareAndExchange:
    case AccessModeTemplate::kCompareAndSet:
      getter.GetReference();  // Skip the comperand.
      new_element = getter.GetReference();
      break;
    case AccessModeTemplate::kGetAndUpdate:
    case AccessModeTemplate::kSet:
      new_element = getter.GetReference();
      break;
  }
  return array->CheckAssignable(new_element);
}

bool ArrayElementVarHandle::Access(AccessMode access_mode,
                                   ShadowFrame* shadow_frame,
                                   const InstructionOperands* const operands,
                                   JValue* result) {
  ShadowFrameGetter getter(*shadow_frame, operands);

  // The target array is the first co-ordinate type preceeding var type arguments.
  ObjPtr<Object> raw_array(getter.GetReference());
  if (raw_array == nullptr) {
    ThrowNullPointerExceptionForCoordinate();
    return false;
  }

  ObjPtr<Array> target_array = raw_array->AsArray();

  // The target array element is the second co-ordinate type preceeding var type arguments.
  const int target_element = getter.Get();
  if (!target_array->CheckIsValidIndex(target_element)) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return false;
  }

  const Primitive::Type primitive_type = GetVarType()->GetPrimitiveType();
  switch (primitive_type) {
    case Primitive::Type::kPrimNot: {
      ObjPtr<ObjectArray<Object>> object_array = target_array->AsObjectArray<Object>();
      if (!CheckArrayStore(access_mode, getter, object_array)) {
        DCHECK(Thread::Current()->IsExceptionPending());
        return false;
      }
      MemberOffset target_element_offset = object_array->OffsetOfElement(target_element);
      return FieldAccessor<ObjPtr<Object>>::Dispatch(access_mode,
                                                     target_array,
                                                     target_element_offset,
                                                     &getter,
                                                     result);
    }
    case Primitive::Type::kPrimBoolean:
      return PrimitiveArrayElementAccessor<uint8_t>::Dispatch(access_mode,
                                                              target_array,
                                                              target_element,
                                                              &getter,
                                                              result);
    case Primitive::Type::kPrimByte:
      return PrimitiveArrayElementAccessor<int8_t>::Dispatch(access_mode,
                                                             target_array,
                                                             target_element,
                                                             &getter,
                                                             result);
    case Primitive::Type::kPrimChar:
      return PrimitiveArrayElementAccessor<uint16_t>::Dispatch(access_mode,
                                                               target_array,
                                                               target_element,
                                                               &getter,
                                                               result);
    case Primitive::Type::kPrimShort:
      return PrimitiveArrayElementAccessor<int16_t>::Dispatch(access_mode,
                                                              target_array,
                                                              target_element,
                                                              &getter,
                                                              result);
    case Primitive::Type::kPrimInt:
      return PrimitiveArrayElementAccessor<int32_t>::Dispatch(access_mode,
                                                              target_array,
                                                              target_element,
                                                              &getter,
                                                              result);
    case Primitive::Type::kPrimLong:
      return PrimitiveArrayElementAccessor<int64_t>::Dispatch(access_mode,
                                                              target_array,
                                                              target_element,
                                                              &getter,
                                                              result);
    case Primitive::Type::kPrimFloat:
      return PrimitiveArrayElementAccessor<float>::Dispatch(access_mode,
                                                            target_array,
                                                            target_element,
                                                            &getter,
                                                            result);
    case Primitive::Type::kPrimDouble:
      return PrimitiveArrayElementAccessor<double>::Dispatch(access_mode,
                                                             target_array,
                                                             target_element,
                                                             &getter,
                                                             result);
    case Primitive::Type::kPrimVoid:
      break;
  }
  LOG(FATAL) << "Unreachable: Unexpected primitive " << primitive_type;
  UNREACHABLE();
}

bool ByteArrayViewVarHandle::GetNativeByteOrder() {
  return GetFieldBoolean(NativeByteOrderOffset());
}

bool ByteArrayViewVarHandle::Access(AccessMode access_mode,
                                    ShadowFrame* shadow_frame,
                                    const InstructionOperands* const operands,
                                    JValue* result) {
  ShadowFrameGetter getter(*shadow_frame, operands);

  // The byte array is the first co-ordinate type preceeding var type arguments.
  ObjPtr<Object> raw_byte_array(getter.GetReference());
  if (raw_byte_array == nullptr) {
    ThrowNullPointerExceptionForCoordinate();
    return false;
  }

  ObjPtr<ByteArray> byte_array(raw_byte_array->AsByteArray());

  // The offset in the byte array element is the second co-ordinate type.
  const int32_t data_offset = getter.Get();

  // Bounds check requested access.
  const Primitive::Type primitive_type = GetVarType()->GetPrimitiveType();
  if (!CheckElementIndex(primitive_type, data_offset, byte_array->GetLength())) {
    return false;
  }

  int8_t* const data = byte_array->GetData();
  bool byte_swap = !GetNativeByteOrder();
  switch (primitive_type) {
    case Primitive::Type::kPrimNot:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimVoid:
      // These are not supported for byte array views and not instantiable.
      break;
    case Primitive::kPrimChar:
      return ByteArrayViewAccessor<uint16_t>::Dispatch(access_mode,
                                                       data,
                                                       data_offset,
                                                       byte_swap,
                                                       &getter,
                                                       result);
    case Primitive::kPrimShort:
      return ByteArrayViewAccessor<int16_t>::Dispatch(access_mode,
                                                      data,
                                                      data_offset,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimInt:
      return ByteArrayViewAccessor<int32_t>::Dispatch(access_mode,
                                                      data,
                                                      data_offset,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimFloat:
      // Treated as a bitwise representation. See javadoc comments for
      // java.lang.invoke.MethodHandles.byteArrayViewVarHandle().
      return ByteArrayViewAccessor<int32_t>::Dispatch(access_mode,
                                                      data,
                                                      data_offset,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimLong:
      return ByteArrayViewAccessor<int64_t>::Dispatch(access_mode,
                                                      data,
                                                      data_offset,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimDouble:
      // Treated as a bitwise representation. See javadoc comments for
      // java.lang.invoke.MethodHandles.byteArrayViewVarHandle().
      return ByteArrayViewAccessor<int64_t>::Dispatch(access_mode,
                                                      data,
                                                      data_offset,
                                                      byte_swap,
                                                      &getter,
                                                      result);
  }
  LOG(FATAL) << "Unreachable: Unexpected primitive " << primitive_type;
  UNREACHABLE();
}

bool ByteBufferViewVarHandle::GetNativeByteOrder() {
  return GetFieldBoolean(NativeByteOrderOffset());
}

bool ByteBufferViewVarHandle::Access(AccessMode access_mode,
                                     ShadowFrame* shadow_frame,
                                     const InstructionOperands* const operands,
                                     JValue* result) {
  ShadowFrameGetter getter(*shadow_frame, operands);

  // The byte buffer is the first co-ordinate argument preceeding var type arguments.
  ObjPtr<Object> byte_buffer(getter.GetReference());
  if (byte_buffer == nullptr) {
    ThrowNullPointerExceptionForCoordinate();
    return false;
  }

  // The byte index for access is the second co-ordinate
  // argument. This is relative to the offset field of the ByteBuffer.
  const int32_t byte_index = getter.Get();

  // Check access_mode is compatible with ByteBuffer's read-only property.
  bool is_read_only = byte_buffer->GetFieldBoolean(
      WellKnownClasses::java_nio_ByteBuffer_isReadOnly->GetOffset());
  if (is_read_only && !IsReadOnlyAccessMode(access_mode)) {
    ThrowReadOnlyBufferException();
    return false;
  }

  // The native_address is only set for ByteBuffer instances backed by native memory.
  const int64_t native_address =
      byte_buffer->GetField64(WellKnownClasses::java_nio_Buffer_address->GetOffset());

  // Determine offset and limit for accesses.
  int32_t byte_buffer_offset;
  if (native_address == 0L) {
    // Accessing a heap allocated byte buffer.
    byte_buffer_offset = byte_buffer->GetField32(
        WellKnownClasses::java_nio_ByteBuffer_offset->GetOffset());
  } else {
    // Accessing direct memory.
    byte_buffer_offset = 0;
  }
  const int32_t byte_buffer_limit =
      byte_buffer->GetField32(WellKnownClasses::java_nio_Buffer_limit->GetOffset());
  const int32_t byte_buffer_length = byte_buffer_offset + byte_buffer_limit;

  const Primitive::Type primitive_type = GetVarType()->GetPrimitiveType();
  if (!CheckElementIndex(primitive_type, byte_index, byte_buffer_offset, byte_buffer_length)) {
    return false;
  }
  const int32_t checked_offset32 = byte_buffer_offset + byte_index;

  int8_t* data;
  if (native_address == 0) {
    ObjPtr<ByteArray> heap_byte_array = byte_buffer->GetFieldObject<ByteArray>(
        WellKnownClasses::java_nio_ByteBuffer_hb->GetOffset());
    data = heap_byte_array->GetData();
  } else {
    data = reinterpret_cast<int8_t*>(static_cast<uint32_t>(native_address));
  }

  bool byte_swap = !GetNativeByteOrder();
  switch (primitive_type) {
    case Primitive::kPrimChar:
      return ByteArrayViewAccessor<uint16_t>::Dispatch(access_mode,
                                                       data,
                                                       checked_offset32,
                                                       byte_swap,
                                                       &getter,
                                                       result);
    case Primitive::kPrimShort:
      return ByteArrayViewAccessor<int16_t>::Dispatch(access_mode,
                                                      data,
                                                      checked_offset32,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimInt:
      return ByteArrayViewAccessor<int32_t>::Dispatch(access_mode,
                                                      data,
                                                      checked_offset32,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimFloat:
      // Treated as a bitwise representation. See javadoc comments for
      // java.lang.invoke.MethodHandles.byteArrayViewVarHandle().
      return ByteArrayViewAccessor<int32_t>::Dispatch(access_mode,
                                                      data,
                                                      checked_offset32,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimLong:
      return ByteArrayViewAccessor<int64_t>::Dispatch(access_mode,
                                                      data,
                                                      checked_offset32,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::kPrimDouble:
      // Treated as a bitwise representation. See javadoc comments for
      // java.lang.invoke.MethodHandles.byteArrayViewVarHandle().
      return ByteArrayViewAccessor<int64_t>::Dispatch(access_mode,
                                                      data,
                                                      checked_offset32,
                                                      byte_swap,
                                                      &getter,
                                                      result);
    case Primitive::Type::kPrimNot:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimVoid:
      // These are not supported for byte array views and not instantiable.
      break;
  }
  LOG(FATAL) << "Unreachable: Unexpected primitive " << primitive_type;
  UNREACHABLE();
}

void FieldVarHandle::VisitTarget(ReflectiveValueVisitor* v) {
  ArtField* orig = GetArtField();
  ArtField* new_value =
      v->VisitField(orig, HeapReflectiveSourceInfo(kSourceJavaLangInvokeFieldVarHandle, this));
  if (orig != new_value) {
    SetArtField(new_value);
  }
}

void StaticFieldVarHandle::VisitTarget(ReflectiveValueVisitor* v) {
  ArtField* orig = GetArtField();
  ArtField* new_value =
      v->VisitField(orig, HeapReflectiveSourceInfo(kSourceJavaLangInvokeFieldVarHandle, this));
  if (orig != new_value) {
    SetArtField(new_value);
    SetFieldObject<false>(DeclaringClassOffset(), new_value->GetDeclaringClass());
  }
}

}  // namespace mirror
}  // namespace art
