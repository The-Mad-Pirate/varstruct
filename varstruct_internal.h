// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VARSTRUCT_VARSTRUCT_INTERNAL_H_
#define VARSTRUCT_VARSTRUCT_INTERNAL_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>
#include <type_traits>
#include <vector>

namespace varstruct_internal {

// Forward declaration needed for VarstructInternal to friend VarstructMember.
class VarstructMember;

// The internal members of Varstruct that need to be accessed by
// VarstructMember.
//
// Without declaring these in a separate class, VarstructMember
// would need to know the template parameters of Varstruct.
class VarstructInternal {
 protected:
  // Bit vector where each bit corresponds to a field in the varstruct and is
  // true if that field is an array, and false otherwise. This is only needed
  // for Varstruct::Create(), so Create() will clear this vector before
  // returning.
  std::vector<bool> arrays_;

  // Also corresponds to a list of fields ordered by declaration.
  // VarstructMember instances append to this vector the sizes of their member
  // types during construction. Varstruct::Create() then modifies these to refer
  // to the offset of the next member (that is, offsets_[0] is the offset of the
  // second member, as the offset of the first member is always 0).
  std::vector<std::size_t> offsets_;
  friend class VarstructMember;
};

// An instance of this class is declared as a field for each varstruct field. It
// stores the unique ascending index of each member. These indices are used for
// the vectors in VarstructInternal.
class VarstructMember {
 public:
  VarstructMember(std::size_t size, bool is_array, VarstructInternal* varstruct)
      : index_(varstruct->offsets_.size()) {
    varstruct->offsets_.push_back(size);
    varstruct->arrays_.push_back(is_array);
  }

  // Index is the index where this member may be found in
  // VarstructInternal::arrays_ and VarstructInternal::offsets_;
  std::size_t index() const { return index_; }

 private:
  std::size_t index_;
};

// A "pointer" type used to indicate that no base pointer was provided.
//
// NoPtr wins by doing absolutely nothing: it is default and copy constructible,
// but contains no members. Its use should generate no code, and should require
// minimal memory usage (C++ requires that every object have an address, so a
// zero-sized object is impossible).
class NoPtr {};

// The base template class of every varstruct.
//
// Base class members shared by all varstructs like Create(), size_bytes(), and
// num_members() are declared here. All other members of subclasses are
// generated by the VARSTRUCT_SCALAR() and VARSTRUCT_ARRAY() macros.
//
// Subclass templates are parameterized on pointer type so that we can support
// mutable pointer, const pointer, and pointerless (offsets only) variants. We
// use a variant of the curiously-recurring template pattern (CRTP) with a
// template template parameter so that we can extract the base template of
// derived members. This allows constructing subclass templates with different
// pointer types, as necessary to make the different Create() factories work.
//
// DEFINE_VARSTRUCT() ensures varstruct classes (the subclasses) specify NoPtr
// as a default template parameter so that the Create() methods may be called
// without template parameters. This and the use of "auto" work to abstract away
// explicit reference to the template parameters from user code.
template <template <typename> class CrtpTemplate, typename PtrType>
class Varstruct : public VarstructInternal {
 public:
  // Create a Varstruct given a void* pointer.
  static CrtpTemplate<void*> Create(void* ptr,
                                    std::deque<std::size_t> array_sizes) {
    return CrtpTemplate<void*>::CreateInternal(ptr, array_sizes);
  }

  // Create a Varstruct given a const void* pointer.
  static CrtpTemplate<const void*> Create(const void* ptr,
                                          std::deque<std::size_t> array_sizes) {
    return CrtpTemplate<const void*>::CreateInternal(ptr, array_sizes);
  }

  // Create a Varstruct without a pointer. Methods that add the pointer to an
  // offset will be disabled.
  static CrtpTemplate<NoPtr> Create(std::deque<std::size_t> array_sizes) {
    return CrtpTemplate<NoPtr>::CreateInternal(NoPtr(), array_sizes);
  }

  // The size in bytes of the entire Varstruct.
  std::size_t size_bytes() const {
    // After Create(), the last offsets_ cell holds the offset immediately after
    // the last member, which is also the size of the entire Varstruct.
    return offsets_.empty() ? 0 : offsets_.back();
  }

  // The number of VARSTRUCT_SCALAR() declarations plus the number of
  // VARSTRUCT_ARRAY() declarations.
  std::size_t num_members() const { return offsets_.size(); }

 protected:
  // The pointer, for instantiations where a void* or const void* was provided.
  // If no pointer was provided to Create(), this will be of type NoPtr and
  // methods defined by macros that operate on ptr_ will be disabled via
  // std::enable_if.
  PtrType ptr_;

 private:
  // Internal creation function called by each Create() overload. The template
  // instantiation parmeters used to invoke this function determine the
  // instantiation variant of the return value.
  static CrtpTemplate<PtrType> CreateInternal(
      PtrType ptr, std::deque<std::size_t> array_sizes);

  // We're friends with other template instantiations. We need this so that
  // CreateInternal() from one template instantiation may be called from another
  // template instantiation. (Recall that NoPtr is used as the default pointer
  // type so that user code doesn't have to concern itself with template
  // parameters).
  template <template <typename> class A, typename B>
  friend class Varstruct;
};

template <template <typename> class CrtpTemplate, typename PtrType>
CrtpTemplate<PtrType> Varstruct<CrtpTemplate, PtrType>::CreateInternal(
    PtrType ptr, std::deque<std::size_t> array_sizes) {
  // All computation of indices and setting of VarstructInternal members
  // occurs upon construction of the varstruct below.
  //
  // After construction, offsets will hold sizes, not the actual offsets. We
  // compute offsets below.
  CrtpTemplate<PtrType> varstruct;

  varstruct.ptr_ = ptr;

  // Multiply each array "offset" (it is currently the type size) by its array
  // size.
  for (int i = 0; i < varstruct.num_members(); i++) {
    if (varstruct.arrays_[i]) {
      assert(!array_sizes.empty());
      varstruct.offsets_[i] *= array_sizes.front();
      array_sizes.pop_front();
    }
  }
  // The number of array_sizes elements should be the same as the number of
  // VARSTRUCT_ARRAY() declarations.
  assert(array_sizes.empty());
  varstruct.arrays_.clear();

  // Now, make the offsets real offsets by carry adding.
  std::size_t total = 0;
  for (int i = 0; i < varstruct.num_members(); i++) {
    total += varstruct.offsets_[i];
    varstruct.offsets_[i] = total;
  }

  return varstruct;
}

// Metafunction to detect NoPtr -- we disable the pointer arithmetic variants
// when using the NoPtr specialization.
template <typename T>
struct IsNoPtr {
  static constexpr bool value = false;
};

template <>
struct IsNoPtr<NoPtr> {
  static constexpr bool value = true;
};

// Metafunction to get either char* or const char*, based on the type of
// PtrType. We convert void* to char* and const void* to const char* in order to
// add the offset and potential array offset to the base pointer.
template <typename PtrType>
struct CharPtrType {
  using type = typename std::conditional<
      std::is_const<typename std::remove_pointer<PtrType>::type>::value,
      const char*, char*>::type;
};

// A constexpr function that performs C-string comparison. Returns true if the
// strings are the same, and false otherwise.
//
// Used for static_asserts concerning varstruct field naming. One of the input
// strings *SHOULD* be fixed and small because the compiler is allowed to limit
// the recursion depth. (Recommended minimum recursion depth limit is 512 calls,
// per Annex B of the C++11 spec).
constexpr bool EqualStrings(const char* a, const char* b) {
  // C++11 constexpr functions must be a single return statement, but recursion
  // is allowed.
  return (*a && *b) ? (*a == *b) && EqualStrings(a + 1, b + 1)
                    : (!(*a) && !(*b));
}

}  // namespace varstruct_internal

// =============================================================================
// Internal macro definitions
// =============================================================================

// An internal macro called by VARSTRUCT_SCALAR_INTERNAL() and
// VARSTRUCT_ARRAY_INTERNAL() that contains the logic shared by both. This macro
// declares the VarstructMember for the field, along with the offset and pointer
// methods.
//
// The pointer method is disabled for the NoPtr template variant, as that
// variant only calculates offsets.
#define VARSTRUCT_DEF_COMMON(decl_type, name, array)                           \
  /* We disallow some problematic varstruct member names. */                   \
  static_assert(!varstruct_internal::EqualStrings(#name, "size_bytes"),        \
                "Cannot name varstruct member 'size_bytes'");                  \
                                                                               \
  static_assert(!varstruct_internal::EqualStrings(#name, "num_members"),       \
                "Cannot name varstruct member 'num_members'");                 \
                                                                               \
  /* The underlying type must be a plain-old data type, since we need to */    \
  /* std::memcpy() to copy the type. */                                        \
  static_assert(std::is_pod<decl_type>::value,                                 \
                "Type '" #decl_type "' is not POD");                           \
                                                                               \
 public:                                                                       \
  /* Returns the offset of a member in the Varstruct. This generated method */ \
  /* is always available, whether a pointer was provided to Create() or */     \
  /* not. */                                                                   \
  std::size_t name##_offset() {                                                \
    const std::size_t index = __##name##_member_.index();                      \
    return (index == 0) ? 0 : this->offsets_[index - 1];                       \
  }                                                                            \
                                                                               \
 private:                                                                      \
  /* Gets a void* or const void* to the given member, with an optional */      \
  /* array index. */                                                           \
  /* We use enable_if to disable this method when NoPtr is used. */            \
  template <bool bounds_check, typename Dummy = char>                          \
  PtrType __##name##__void__ptr__(                                             \
      std::size_t array_index = 0,                                             \
      typename std::enable_if<!varstruct_internal::IsNoPtr<PtrType>::value,    \
                              Dummy>::type* = 0) {                             \
    if (bounds_check) {                                                        \
      const std::size_t array_elems = name##_size() / sizeof(decl_type);       \
      assert(array_index >= 0 && array_index < array_elems);                   \
    }                                                                          \
    return static_cast<                                                        \
               typename varstruct_internal::CharPtrType<PtrType>::type>(       \
               this->ptr_) +                                                   \
           name##_offset() + array_index * sizeof(decl_type);                  \
  }                                                                            \
                                                                               \
  /* The declaration of the VarstructMember for this field. This */            \
  /* declaration allows Varstruct::CreateInternal() to read the size of */     \
  /* field, along with whether it is an array or not. */                       \
  varstruct_internal::VarstructMember __##name##_member_{sizeof(decl_type),    \
                                                         array, this};

#define DEFINE_VARSTRUCT_INTERNAL(name)                                   \
  /* Forward declare the user varstruct class and declare a using */      \
  /* statement. This allows the user to call their varstruct's static */  \
  /* methods without a template qualifier like <> at the end. */          \
  template <typename PtrType = varstruct_internal::NoPtr>                 \
  class name##_template;                                                  \
  using name = name##_template<>;                                         \
                                                                          \
  /* This is the beginning of the actual Varstruct definition that the */ \
  /* libary consumer will fill out. An open brace with the declared */    \
  /* members is expected to follow. We use a variant of the */            \
  /* curiously-recurring template pattern with template template */       \
  /* parameters to allow mutable pointer, const pointer, and  */          \
  /* pointerless (offsets-only) variants. User code should *NOT* */       \
  /* manually specify its own PtrType. */                                 \
  template <typename PtrType>                                             \
  class name##_template                                                   \
      : public varstruct_internal::Varstruct<name##_template, PtrType>

#define VARSTRUCT_SCALAR_INTERNAL(decl_type, name)                             \
  VARSTRUCT_DEF_COMMON(decl_type, name, false)                                 \
                                                                               \
 public:                                                                       \
  /* Returns the total size in bytes of the scalar. */                         \
  static std::size_t name##_size() { return sizeof(decl_type); }               \
                                                                               \
  /* Reads from the scalar. We use enable_if to disable this method when */    \
  /* NoPtr is used. */                                                         \
  template <typename Dummy = char>                                             \
  decl_type name(                                                              \
      typename std::enable_if<!varstruct_internal::IsNoPtr<PtrType>::value,    \
                              Dummy>::type* = 0) {                             \
    constexpr bool kBoundsCheck = false;                                       \
    decl_type tmp;                                                             \
    std::memcpy(&tmp, __##name##__void__ptr__<kBoundsCheck>(), sizeof(tmp));   \
    return tmp;                                                                \
  }                                                                            \
                                                                               \
  /* Writes to the scalar. We use enable_if to disable this method when */     \
  /* NoPtr is used or if the pointer was to const. */                          \
  template <typename Dummy = char>                                             \
  void set_##name(                                                             \
      decl_type new_value,                                                     \
      typename std::enable_if<!varstruct_internal::IsNoPtr<PtrType>::value &&  \
                                  !std::is_const<typename std::remove_pointer< \
                                      PtrType>::type>::value,                  \
                              Dummy>::type* = 0) {                             \
    constexpr bool kBoundsCheck = false;                                       \
    std::memcpy(__##name##__void__ptr__<kBoundsCheck>(), &new_value,           \
                sizeof(new_value));                                            \
  }

#define VARSTRUCT_ARRAY_INTERNAL(decl_type, name)                              \
  VARSTRUCT_DEF_COMMON(decl_type, name, true)                                  \
                                                                               \
 public:                                                                       \
  /* Returns the total size in bytes of all array elements. */                 \
  std::size_t name##_size() {                                                  \
    const std::size_t index = __##name##_member_.index();                      \
    if (index == 0) {                                                          \
      return this->offsets_[index];                                            \
    } else {                                                                   \
      return this->offsets_[index] - this->offsets_[index - 1];                \
    }                                                                          \
  }                                                                            \
                                                                               \
  /* Reads and returns an element of the array. Performs bounds checking if */ \
  /* the bounds_check template parameter is true (defaults to true). */        \
  /* We use enable_if to disable this method when NoPtr is used. */            \
  template <bool bounds_check = true, typename Dummy = char>                   \
  decl_type name(                                                              \
      std::size_t array_index,                                                 \
      typename std::enable_if<!varstruct_internal::IsNoPtr<PtrType>::value,    \
                              Dummy>::type* = 0) {                             \
    decl_type tmp;                                                             \
    std::memcpy(&tmp, __##name##__void__ptr__<bounds_check>(array_index),      \
                sizeof(tmp));                                                  \
    return tmp;                                                                \
  }                                                                            \
                                                                               \
  /* Writes to an element of the array. Performs bounds checking if */         \
  /* the bounds_check template parameter is true (defaults to true). */        \
  /* We use enable_if to disable this method when NoPtr is used or if the */   \
  /* pointer was to const. */                                                  \
  template <bool bounds_check = true, typename Dummy = char>                   \
  void set_##name(                                                             \
      std::size_t array_index, decl_type new_value,                            \
      typename std::enable_if<!varstruct_internal::IsNoPtr<PtrType>::value &&  \
                                  !std::is_const<typename std::remove_pointer< \
                                      PtrType>::type>::value,                  \
                              Dummy>::type* = 0) {                             \
    std::memcpy(__##name##__void__ptr__<bounds_check>(array_index),            \
                &new_value, sizeof(new_value));                                \
  }

#endif  // VARSTRUCT_VARSTRUCT_INTERNAL_H_
