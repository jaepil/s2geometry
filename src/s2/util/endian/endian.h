// Copyright 2005 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

//
// Utility functions that depend on bytesex. We define htonll and ntohll,
// as well as "Google" versions of all the standards: ghtonl, ghtons, and
// so on. These functions do exactly the same as their standard variants,
// but don't require including the dangerous netinet/in.h.
//
// Buffer routines will copy to and from buffers without causing
// a bus error when the architecture requires different byte alignments.
#ifndef S2_UTIL_ENDIAN_ENDIAN_H_
#define S2_UTIL_ENDIAN_ENDIAN_H_

#include <cassert>

#include <cstdint>
#include <type_traits>

#include "s2/base/port.h"
#include "s2/base/types.h"
#include "absl/base/casts.h"
#include "absl/base/config.h"
#include "absl/base/internal/endian.h"
#include "absl/base/optimization.h"
#include "absl/base/port.h"
// TODO(b/329676208): remove unused log.h
#include "absl/log/absl_log.h"  // IWYU pragma: keep
#include "absl/numeric/int128.h"
#include "s2/util/gtl/unaligned.h"

inline uint64 gbswap_64(uint64 host_int) {
  return absl::gbswap_64(host_int);
}

inline uint32 gbswap_32(uint32 host_int) {
  return absl::gbswap_32(host_int);
}

inline uint16 gbswap_16(uint16 host_int) {
  return absl::gbswap_16(host_int);
}

inline absl::uint128 gbswap_128(absl::uint128 host_int) {
  return absl::MakeUint128(gbswap_64(absl::Uint128Low64(host_int)),
                           gbswap_64(absl::Uint128High64(host_int)));
}

#ifdef IS_LITTLE_ENDIAN

// Definitions for ntohl etc. that don't require us to include
// netinet/in.h. We wrap gbswap_32 and gbswap_16 in functions rather
// than just #defining them because in debug mode, gcc doesn't
// correctly handle the (rather involved) definitions of bswap_32.
// gcc guarantees that inline functions are as fast as macros, so
// this isn't a performance hit.
inline uint16 ghtons(uint16 x) { return gbswap_16(x); }
inline uint32 ghtonl(uint32 x) { return gbswap_32(x); }
inline uint64 ghtonll(uint64 x) { return gbswap_64(x); }

#elif defined IS_BIG_ENDIAN

// These definitions are simpler on big-endian machines
// These are functions instead of macros to avoid self-assignment warnings
// on calls such as "i = ghtnol(i);".  This also provides type checking.
inline uint16 ghtons(uint16 x) { return x; }
inline uint32 ghtonl(uint32 x) { return x; }
inline uint64 ghtonll(uint64 x) { return x; }

#else
#error "Unsupported bytesex: Either IS_BIG_ENDIAN or IS_LITTLE_ENDIAN must be defined"  // NOLINT
#endif  // bytesex

#ifndef htonll
// With the rise of 64-bit, some systems are beginning to define this.
#define htonll(x) ghtonll(x)
#endif  // htonll

// ntoh* and hton* are the same thing for any size and bytesex,
// since the function is an involution, i.e., its own inverse.
inline uint16 gntohs(uint16 x) { return ghtons(x); }
inline uint32 gntohl(uint32 x) { return ghtonl(x); }
inline uint64 gntohll(uint64 x) { return ghtonll(x); }

#ifndef ntohll
#define ntohll(x) htonll(x)
#endif  // ntohll

// We provide unified FromHost and ToHost APIs for all integral types and float,
// double types. If variable v's type is known to be one of these types, the
// client can simply call the following function without worrying about its
// return type.
//     LittleEndian::FromHost(v), or BigEndian::FromHost(v)
//     LittleEndian::ToHost(v), or BigEndian::ToHost(v)
// This unified FromHost and ToHost APIs are useful inside a template when the
// type of v is a template parameter.
//
// In order to unify all "IntType FromHostxx(ValueType)" and "IntType
// ToHostxx(ValueType)" APIs, we use the following trait class to automatically
// find the corresponding IntType given a ValueType, where IntType is an
// unsigned integer type with the same size of ValueType. The supported
// ValueTypes are uint8, uint16, uint32, uint64, int8, int16, int32, int64,
// bool, float, double.
//
// template <class ValueType>
// struct tofromhost_value_type_traits {
//   typedef ValueType value_type;
//   typedef IntType int_type;
// }
//
// We don't provide the default implementation for this trait struct.
// So that if ValueType is not supported by the FromHost and ToHost APIs, it
// will give a compile time error.
template <class ValueType>
struct tofromhost_value_type_traits;

// General byte order converter class template. It provides a common
// implementation for LittleEndian::FromHost(ValueType),
// BigEndian::FromHost(ValueType), LittleEndian::ToHost(ValueType), and
// BigEndian::ToHost(ValueType).
template <class EndianClass, typename ValueType>
class GeneralFormatConverter {
 public:
  static typename tofromhost_value_type_traits<ValueType>::int_type FromHost(
      ValueType v);
  static typename tofromhost_value_type_traits<ValueType>::int_type ToHost(
      ValueType v);
};

// Utilities to convert numbers between the current hosts's native byte
// order and little-endian byte order
//
// Load/Store methods are alignment safe
class LittleEndian {
 public:
  // Conversion functions.
#ifdef IS_LITTLE_ENDIAN

  static uint16 FromHost16(uint16 x) { return x; }
  static uint16 ToHost16(uint16 x) { return x; }

  static uint32 FromHost32(uint32 x) { return x; }
  static uint32 ToHost32(uint32 x) { return x; }

  static uint64 FromHost64(uint64 x) { return x; }
  static uint64 ToHost64(uint64 x) { return x; }

  static absl::uint128 FromHost128(absl::uint128 x) { return x; }
  static absl::uint128 ToHost128(absl::uint128 x) { return x; }

  static constexpr bool IsLittleEndian() { return true; }

#elif defined IS_BIG_ENDIAN

  static uint16 FromHost16(uint16 x) { return gbswap_16(x); }
  static uint16 ToHost16(uint16 x) { return gbswap_16(x); }

  static uint32 FromHost32(uint32 x) { return gbswap_32(x); }
  static uint32 ToHost32(uint32 x) { return gbswap_32(x); }

  static uint64 FromHost64(uint64 x) { return gbswap_64(x); }
  static uint64 ToHost64(uint64 x) { return gbswap_64(x); }

  static absl::uint128 FromHost128(absl::uint128 x) { return gbswap_128(x); }
  static absl::uint128 ToHost128(absl::uint128 x) { return gbswap_128(x); }

  static constexpr bool IsLittleEndian() { return false; }

#endif /* ENDIAN */

  // Unified LittleEndian::FromHost(ValueType v) API.
  template <class ValueType>
  static typename tofromhost_value_type_traits<ValueType>::int_type FromHost(
      ValueType v) {
    return GeneralFormatConverter<LittleEndian, ValueType>::FromHost(v);
  }

  // Unified LittleEndian::ToHost(ValueType v) API.
  template <class ValueType>
  static typename tofromhost_value_type_traits<ValueType>::value_type ToHost(
      ValueType v) {
    return GeneralFormatConverter<LittleEndian, ValueType>::ToHost(v);
  }

  // Functions to do unaligned loads and stores in little-endian order.
  template <typename T, size_t N>
  static uint16 Load16(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint16),
                  "Not enough space in buffer to load value from");
    return Load16(static_cast<const void*>(p));
  }

  template <typename T = void*, size_t N = 0>
  static uint16 Load16(const void* p) {
    return ToHost16(UNALIGNED_LOAD16(p));
  }

  template <typename T, size_t N>
  static void Store16(T (&p)[N], uint16 v) {
    static_assert(sizeof(T) * N >= sizeof(uint16),
                  "Not enough space in buffer to store value");
    return Store16(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store16(void* p, uint16 v) {
    UNALIGNED_STORE16(p, FromHost16(v));
  }

  template <typename T, size_t N>
  static uint32 Load24(T (&p)[N]) {
    static_assert(sizeof(T) * N >= 3,
                  "Not enough space in buffer to load value from");
    return Load24(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint32 Load24(const void* p) {
#ifdef IS_LITTLE_ENDIAN
    uint32 result = 0;
    memcpy(&result, p, 3);
    return result;
#else
    const uint8* data = reinterpret_cast<const uint8*>(p);
    return Load16(data) + (data[2] << 16);
#endif
  }

  template <typename T, size_t N>
  static void Store24(T (&p)[N], uint32 v) {
    static_assert(sizeof(T) * N >= 3,
                  "Not enough space in buffer to store value");
    return Store24(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store24(void* p, uint32 v) {
#ifdef IS_LITTLE_ENDIAN
    memcpy(p, &v, 3);
#else
    uint8* data = reinterpret_cast<uint8*>(p);
    data[0] = v & 0xFF;
    data[1] = (v >> 8) & 0xFF;
    data[2] = (v >> 16) & 0xFF;
#endif
  }

  template <typename T, size_t N>
  static uint32 Load32(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint32),
                  "Not enough space in buffer to load value from");
    return Load32(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint32 Load32(const void* p) {
    return ToHost32(UNALIGNED_LOAD32(p));
  }

  template <typename T, size_t N>
  static void Store32(T (&p)[N], uint32 v) {
    static_assert(sizeof(T) * N >= sizeof(uint32),
                  "Not enough space in buffer to store value");
    Store32(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store32(void* p, uint32 v) {
    UNALIGNED_STORE32(p, FromHost32(v));
  }

  template <typename T, size_t N>
  static uint64 Load64(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint64),
                  "Not enough space in buffer to load value from");
    return Load64(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint64 Load64(const void* p) {
    return ToHost64(UNALIGNED_LOAD64(p));
  }

  // Build a uint64 from 1-8 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // LittleEndian order. The 64 - 8 * len most significant bits are
  // set all to 0.
  // In latex-friendly words, this function returns:
  //     $\sum_{i=0}^{len-1} p[i] 256^{i}$, where p[i] is unsigned.
  //
  // This function is equivalent to:
  // uint64 val = 0;
  // memcpy(&val, p, len);
  // return ToHost64(val);
  //
  // The caller needs to guarantee that 0 <= len <= 8.
  static uint64 Load64VariableLength(const void* const p, int len) {
    ABSL_ASSUME(len >= 0 && len <= 8);
    uint64 val = 0;
    const uint8* const src = static_cast<const uint8*>(p);
    for (int i = 0; i < len; ++i) {
      val |= static_cast<uint64>(src[i]) << (8 * i);
    }
    return val;
  }

  // Store the least significant 1-8 bytes of a uint64.
  // 8 * len least significant bits are loaded from the given uint64 and written
  // to the provided buffer in LittleEndian order. The 64 - 8 * len most
  // significant bits are ignored.
  //
  // The caller needs to guarantee that 0 <= len <= 8.
  static void Store64VariableLength(void* const p, uint64 v, int len) {
    assert(len >= 0 && len <= 8);
    v = FromHost64(v);
    memcpy(p, &v, len);
  }

  template <typename T, size_t N>
  static void Store64(T (&p)[N], uint64 v) {
    static_assert(sizeof(T) * N >= sizeof(uint64),
                  "Not enough space in buffer to store value");
    Store64(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store64(void* p, uint64 v) {
    UNALIGNED_STORE64(p, FromHost64(v));
  }

  template <typename T, size_t N>
  static absl::uint128 Load128(T (&p)[N]) {
    static_assert(sizeof(T) * N >= 16,
                  "Not enough space in buffer to load value from");
    return Load128(static_cast<const void*>(p));
  }

  template <typename T = void*, size_t N = 0>
  static absl::uint128 Load128(const void* p) {
    return absl::MakeUint128(
        ToHost64(UNALIGNED_LOAD64(reinterpret_cast<const uint64*>(p) + 1)),
        ToHost64(UNALIGNED_LOAD64(p)));
  }

  template <typename T, size_t N>
  static void Store128(T (&p)[N], const absl::uint128 v) {
    static_assert(sizeof(T) * N >= 16,
                  "Not enough space in buffer to store value");
    return Store128(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store128(void* p, const absl::uint128 v) {
    UNALIGNED_STORE64(p, FromHost64(absl::Uint128Low64(v)));
    UNALIGNED_STORE64(reinterpret_cast<uint64*>(p) + 1,
                      FromHost64(absl::Uint128High64(v)));
  }

  // Build a uint128 from 1-16 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // LittleEndian order. The 128 - 8 * len most significant bits are
  // set all to 0.
  static absl::uint128 Load128VariableLength(const void* p, int len) {
    if (len <= 8) {
      return absl::uint128(Load64VariableLength(p, len));
    } else {
      return absl::MakeUint128(
          Load64VariableLength(static_cast<const char*>(p) + 8, len - 8),
          Load64(p));
    }
  }

  // Load & Store in machine's word size.
  template <typename T, size_t N>
  static uword_t LoadUnsignedWord(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uword_t),
                  "Not enough space in buffer to load value from");
    return LoadUnsignedWord(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uword_t LoadUnsignedWord(const void* p) {
    if constexpr (sizeof(uword_t) == 8)
      return Load64(p);
    else
      return Load32(p);
  }

  template <typename T, size_t N>
  static void StoreUnsignedWord(T (&p)[N], uword_t v) {
    static_assert(sizeof(T) * N <= sizeof(uword_t),
                  "Not enough space in buffer to store value");
    StoreUnsignedWord(static_cast<void*>(p), v);
  }

  template <typename T = const void*, size_t N = 0>
  static void StoreUnsignedWord(void* p, uword_t v) {
    if constexpr (sizeof(v) == 8)
      Store64(p, v);
    else
      Store32(p, v);
  }

  // Unified LittleEndian::Load/Store<T> API.

  // Returns the T value encoded by the leading bytes of 'p', interpreted
  // according to the format specified below. 'p' has no alignment restrictions.
  //
  // Type              Format
  // ----------------  -------------------------------------------------------
  // uint{8,16,32,64}  Little-endian binary representation.
  // int{8,16,32,64}   Little-endian twos-complement binary representation.
  // float,double      Little-endian IEEE-754 format.
  // char              The raw byte.
  // bool              A byte. 0 maps to false; all other values map to true.
  template<typename T>
  static T Load(const char* p);

  // Encodes 'value' in the format corresponding to T. Supported types are
  // described in Load<T>(). 'p' has no alignment restrictions. In-place Store
  // is safe (that is, it is safe to call
  // Store(x, reinterpret_cast<char*>(&x))).
  template<typename T>
  static void Store(T value, char* p);
};

// Utilities to convert numbers between the current hosts's native byte
// order and big-endian byte order (same as network byte order)
//
// Load/Store methods are alignment safe
class BigEndian {
 public:
#ifdef IS_LITTLE_ENDIAN

  static uint16 FromHost16(uint16 x) { return gbswap_16(x); }
  static uint16 ToHost16(uint16 x) { return gbswap_16(x); }

  static uint32 FromHost32(uint32 x) { return gbswap_32(x); }
  static uint32 ToHost32(uint32 x) { return gbswap_32(x); }

  static uint64 FromHost64(uint64 x) { return gbswap_64(x); }
  static uint64 ToHost64(uint64 x) { return gbswap_64(x); }

  static absl::uint128 FromHost128(absl::uint128 x) { return gbswap_128(x); }
  static absl::uint128 ToHost128(absl::uint128 x) { return gbswap_128(x); }

  static constexpr bool IsLittleEndian() { return true; }

#elif defined IS_BIG_ENDIAN

  static uint16 FromHost16(uint16 x) { return x; }
  static uint16 ToHost16(uint16 x) { return x; }

  static uint32 FromHost32(uint32 x) { return x; }
  static uint32 ToHost32(uint32 x) { return x; }

  static uint64 FromHost64(uint64 x) { return x; }
  static uint64 ToHost64(uint64 x) { return x; }

  static absl::uint128 FromHost128(absl::uint128 x) { return x; }
  static absl::uint128 ToHost128(absl::uint128 x) { return x; }

  static constexpr bool IsLittleEndian() { return false; }

#endif /* ENDIAN */

  // Unified BigEndian::FromHost(ValueType v) API.
  template <class ValueType>
  static typename tofromhost_value_type_traits<ValueType>::int_type FromHost(
      ValueType v) {
    return GeneralFormatConverter<BigEndian, ValueType>::FromHost(v);
  }

  // Unified BigEndian::ToHost(ValueType v) API.
  template <class ValueType>
  static typename tofromhost_value_type_traits<ValueType>::value_type ToHost(
      ValueType v) {
    return GeneralFormatConverter<BigEndian, ValueType>::ToHost(v);
  }

  // Functions to do unaligned loads and stores in big-endian order.
  template <typename T, size_t N>
  static uint16 Load16(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint16),
                  "Not enough space in buffer to load value from");
    return Load16(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint16 Load16(const void* p) {
    return ToHost16(UNALIGNED_LOAD16(p));
  }

  template <typename T, size_t N>
  static void Store16(T (&p)[N], uint16 v) {
    static_assert(sizeof(T) * N >= sizeof(uint16),
                  "Not enough space in buffer to store value");
    return Store16(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store16(void* p, uint16 v) {
    UNALIGNED_STORE16(p, FromHost16(v));
  }

  template <typename T, size_t N>
  static uint32 Load24(T (&p)[N]) {
    static_assert(sizeof(T) * N >= 3,
                  "Not enough space in buffer to load value from");
    return Load24(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint32 Load24(const void* p) {
    const uint8* data = reinterpret_cast<const uint8*>(p);
    return (data[0] << 16) + Load16(data + 1);
  }

  template <typename T, size_t N>
  static void Store24(T (&p)[N], uint32 v) {
    static_assert(sizeof(T) * N >= 3,
                  "Not enough space in buffer to store value");
    return Store24(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store24(void* p, uint32 v) {
    uint8* data = reinterpret_cast<uint8*>(p);
    Store16(data + 1, static_cast<uint16>(v));
    *data = static_cast<uint8>(v >> 16);
  }

  template <typename T, size_t N>
  static uint32 Load32(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint32),
                  "Not enough space in buffer to load value from");
    return ToHost32(UNALIGNED_LOAD32(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uint32 Load32(const void* p) {
    return ToHost32(UNALIGNED_LOAD32(p));
  }

  template <typename T, size_t N>
  static void Store32(T (&x)[N], uint32 v) {
    static_assert(sizeof(T) * N >= sizeof(uint32),
                  "Not enough space in buffer to store value");
    UNALIGNED_STORE32(x, FromHost32(v));
  }

  template <typename T = void*, size_t N = 0>
  static void Store32(void* p, uint32 v) {
    UNALIGNED_STORE32(p, FromHost32(v));
  }

  template <typename T, size_t N>
  static uint64 Load64(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uint64),
                  "Not enough space in buffer to load value from");
    return Load64(static_cast<const void*>(p));
  }

  static uint64 Load64(const void* p) {
    return ToHost64(UNALIGNED_LOAD64(p));
  }

  // Semantically build a uint64 from 1-8 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // BigEndian order. The 64 - 8 * len most significant bits are
  // set all to 0.
  // In latex-friendly words, this function returns:
  //     $\sum_{i=0}^{len-1} p[i] 256^{i}$, where p[i] is unsigned.
  //
  // This function is equivalent to:
  // uint64 val = 0;
  // memcpy(&val, p, len);
  // return ToHost64(val);
  //
  // The caller needs to guarantee that 0 <= len <= 8.
  static uint64 Load64VariableLength(const void* const p, int len) {
    assert(len >= 0 && len <= 8);
    uint64 val = 0;
    const uint8* const src = static_cast<const uint8*>(p);
    for (int i = 0; i < 8; ++i) {
      if (i < len) {
        val = (val << 8) | src[i];
      }
    }
    return val;
  }

  // Store the least significant 1-8 bytes of a uint64.
  // 8 * len least significant bits are loaded from the given uint64 and written
  // to the provided buffer in BigEndian order. The 64 - 8 * len most
  // significant bits are ignored.
  //
  // The caller needs to guarantee that 0 <= len <= 8.
  static void Store64VariableLength(void* const p, uint64 v, int len) {
    assert(len >= 0 && len <= 8);
    v = FromHost64(v);
    memcpy(p, reinterpret_cast<uint8*>(&v) + sizeof(uint64) - len, len);
  }

  template <typename T, size_t N>
  static void Store64(T (&p)[N], uint64 v) {
    static_assert(sizeof(T) * N >= sizeof(uint64),
                  "Not enough space in buffer to store value");
    return Store64(static_cast<void*>(p), v);
  }

  template <typename T = void*, size_t N = 0>
  static void Store64(void* p, uint64 v) {
    UNALIGNED_STORE64(p, FromHost64(v));
  }

  static absl::uint128 Load128(const void* p) {
    return absl::MakeUint128(
        ToHost64(UNALIGNED_LOAD64(p)),
        ToHost64(UNALIGNED_LOAD64(reinterpret_cast<const uint64*>(p) + 1)));
  }

  static void Store128(void* p, const absl::uint128 v) {
    UNALIGNED_STORE64(p, FromHost64(absl::Uint128High64(v)));
    UNALIGNED_STORE64(reinterpret_cast<uint64*>(p) + 1,
                      FromHost64(absl::Uint128Low64(v)));
  }

  // Build a uint128 from 1-16 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // BigEndian order. The 128 - 8 * len most significant bits are
  // set all to 0.
  static absl::uint128 Load128VariableLength(const void* p, int len) {
    if (len <= 8) {
      return absl::uint128(
          Load64VariableLength(static_cast<const char*>(p), len));
    } else if (len < 16) {
      return absl::MakeUint128(Load64VariableLength(p, len - 8),
                               Load64(static_cast<const char*>(p) + len - 8));
    } else {
      return absl::MakeUint128(Load64(static_cast<const char*>(p)),
                               Load64(static_cast<const char*>(p) + 8));
    }
  }

  // Load & Store in machine's word size.
  template <typename T, size_t N>
  static uword_t LoadUnsignedWord(T (&p)[N]) {
    static_assert(sizeof(T) * N >= sizeof(uword_t),
                  "Not enough space in buffer to load value from");
    return LoadUnsignedWord(static_cast<const void*>(p));
  }

  template <typename T = const void*, size_t N = 0>
  static uword_t LoadUnsignedWord(const void* p) {
    if constexpr (sizeof(uword_t) == 8)
      return Load64(p);
    else
      return Load32(p);
  }

  template <typename T, size_t N>
  static void StoreUnsignedWord(T (&p)[N], uword_t v) {
    static_assert(sizeof(T) * N >= sizeof(uword_t),
                  "Not enough space in buffer to store value");
    return StoreUnsignedWord(static_cast<void*>(p), v);
  }

  template <typename T = const void*, size_t N = 0>
  static void StoreUnsignedWord(void* p, uword_t v) {
    if constexpr (sizeof(uword_t) == 8)
      Store64(p, v);
    else
      Store32(p, v);
  }

  // Unified BigEndian::Load/Store<T> API.

  // Returns the T value encoded by the leading bytes of 'p', interpreted
  // according to the format specified below. 'p' has no alignment restrictions.
  //
  // Type              Format
  // ----------------  -------------------------------------------------------
  // uint{8,16,32,64}  Big-endian binary representation.
  // int{8,16,32,64}   Big-endian twos-complement binary representation.
  // float,double      Big-endian IEEE-754 format.
  // char              The raw byte.
  // bool              A byte. 0 maps to false; all other values map to true.
  template<typename T>
  static T Load(const char* p);

  // Encodes 'value' in the format corresponding to T. Supported types are
  // described in Load<T>(). 'p' has no alignment restrictions. In-place Store
  // is safe (that is, it is safe to call
  // Store(x, reinterpret_cast<char*>(&x))).
  template<typename T>
  static void Store(T value, char* p);
};  // BigEndian

// Network byte order is big-endian
typedef BigEndian NetworkByteOrder;

//////////////////////////////////////////////////////////////////////
// Implementation details: Clients can stop reading here.
//
// Define ValueType->IntType mapping for the unified
// "IntType FromHost(ValueType)" API. The mapping is implemented via
// tofromhost_value_type_traits trait struct. Every legal ValueType has its own
// specialization. There is no default body for this trait struct, so that
// any type that is not supported by the unified FromHost API
// will trigger a compile time error.
#define FROMHOST_TYPE_MAP(ITYPE, VTYPE)        \
  template <>                                  \
  struct tofromhost_value_type_traits<VTYPE> { \
    typedef VTYPE value_type;                  \
    typedef ITYPE int_type;                    \
  }

FROMHOST_TYPE_MAP(uint8, uint8);
FROMHOST_TYPE_MAP(uint8, int8);
FROMHOST_TYPE_MAP(uint16, uint16);
FROMHOST_TYPE_MAP(uint16, int16);
FROMHOST_TYPE_MAP(uint32, uint32);
FROMHOST_TYPE_MAP(uint32, int32);
FROMHOST_TYPE_MAP(uint64, uint64);
FROMHOST_TYPE_MAP(uint64, int64);
FROMHOST_TYPE_MAP(uint32, float);
FROMHOST_TYPE_MAP(uint64, double);
FROMHOST_TYPE_MAP(uint8, bool);
FROMHOST_TYPE_MAP(absl::uint128, absl::uint128);
#undef FROMHOST_TYPE_MAP

// Default implementation for the unified FromHost(ValueType) API, which
// handles all integral types (ValueType is one of uint8, int8, uint16, int16,
// uint32, int32, uint64, int64). The compiler will remove the switch case
// branches and unnecessary static_cast, when the template is expanded.
template <class EndianClass, typename ValueType>
typename tofromhost_value_type_traits<ValueType>::int_type
GeneralFormatConverter<EndianClass, ValueType>::FromHost(ValueType v) {
  if constexpr (sizeof(ValueType) == sizeof(uint8)) {
    return static_cast<uint8>(v);
  } else if constexpr (sizeof(ValueType) == sizeof(uint16)) {
    return EndianClass::FromHost16(static_cast<uint16>(v));
  } else if constexpr (sizeof(ValueType) == sizeof(uint32)) {
    return EndianClass::FromHost32(static_cast<uint32>(v));
  } else {
    static_assert(sizeof(ValueType) == sizeof(uint64),
                  "ValueType must be 8, 16, 32, or 64 bits");
    return EndianClass::FromHost64(static_cast<uint64>(v));
  }
}

// Default implementation for the unified ToHost(ValueType) API, which handles
// all integral types (ValueType is one of uint8, int8, uint16, int16, uint32,
// int32, uint64, int64). The compiler will remove the switch case branches and
// unnecessary static_cast, when the template is expanded.
template <class EndianClass, typename ValueType>
typename tofromhost_value_type_traits<ValueType>::int_type
GeneralFormatConverter<EndianClass, ValueType>::ToHost(ValueType v) {
  if constexpr (sizeof(ValueType) == sizeof(uint8)) {
    return static_cast<uint8>(v);
  } else if constexpr (sizeof(ValueType) == sizeof(uint16)) {
    return EndianClass::ToHost16(static_cast<uint16>(v));
  } else if constexpr (sizeof(ValueType) == sizeof(uint32)) {
    return EndianClass::ToHost32(static_cast<uint32>(v));
  } else {
    static_assert(sizeof(ValueType) == sizeof(uint64),
                  "ValueType must be 8, 16, 32, or 64 bits");
    return EndianClass::ToHost64(static_cast<uint64>(v));
  }
}

// Specialization of the unified FromHost(ValueType) API, which handles
// float types (ValueType is float).
template <class EndianClass>
class GeneralFormatConverter<EndianClass, float> {
 public:
  static typename tofromhost_value_type_traits<float>::int_type FromHost(
      float v) {
    return EndianClass::FromHost32(absl::bit_cast<uint32>(v));
  }
  static typename tofromhost_value_type_traits<float>::int_type ToHost(
      float v) {
    return absl::bit_cast<float>(
        EndianClass::ToHost32(absl::bit_cast<uint32>(v)));
  }
};

// Specialization of the unified FromHost(ValueType) API, which handles
// double types (ValueType is double).
template <class EndianClass>
class GeneralFormatConverter<EndianClass, double> {
 public:
  static typename tofromhost_value_type_traits<double>::int_type FromHost(
      double v) {
    return EndianClass::FromHost64(absl::bit_cast<uint64>(v));
  }
  static typename tofromhost_value_type_traits<double>::int_type ToHost(
      double v) {
    return absl::bit_cast<double>(
        EndianClass::ToHost64(absl::bit_cast<uint64>(v)));
  }
};

// Specialization of the unified FromHost(ValueType) API, which handles
// uint128 types (ValueType is uint128).
template <class EndianClass>
class GeneralFormatConverter<EndianClass, absl::uint128> {
 public:
  static typename tofromhost_value_type_traits<absl::uint128>::int_type
  FromHost(absl::uint128 v) {
    return EndianClass::FromHost128(v);
  }
  static typename tofromhost_value_type_traits<absl::uint128>::int_type ToHost(
      absl::uint128 v) {
    return EndianClass::ToHost128(v);
  }
};

namespace endian_internal {
// Integer helper methods for the unified Load/Store APIs.

// Which branch of the 'case' to use is decided at compile time, so despite the
// apparent size of this function, it compiles into efficient code.
template<typename EndianClass, typename T>
inline T LoadInteger(const char* p) {
  if constexpr (sizeof(T) == sizeof(uint8)) {
    return gtl::UnalignedLoad<T>(p);
  } else if constexpr (sizeof(T) == sizeof(uint16)) {
    return EndianClass::ToHost16(UNALIGNED_LOAD16(p));
  } else if constexpr (sizeof(T) == sizeof(uint32)) {
    return EndianClass::ToHost32(UNALIGNED_LOAD32(p));
  } else {
    static_assert(sizeof(T) == sizeof(uint64),
                  "T must be 8, 16, 32, or 64 bits");
    return EndianClass::ToHost64(UNALIGNED_LOAD64(p));
  }
}

// Which branch of the 'case' to use is decided at compile time, so despite the
// apparent size of this function, it compiles into efficient code.
template<typename EndianClass, typename T>
inline void StoreInteger(T value, char* p) {
  if constexpr (sizeof(T) == sizeof(uint8)) {
    *reinterpret_cast<T*>(p) = value;
  } else if constexpr (sizeof(T) == sizeof(uint16)) {
    UNALIGNED_STORE16(p, EndianClass::FromHost16(value));
  } else if constexpr (sizeof(T) == sizeof(uint32)) {
    UNALIGNED_STORE32(p, EndianClass::FromHost32(value));
  } else {
    static_assert(sizeof(T) == sizeof(uint64),
                  "T must be 8, 16, 32, or 64 bits");
    UNALIGNED_STORE64(p, EndianClass::FromHost64(value));
  }
}

// Floating point helper methods for the unified Load/Store APIs.

template<typename EndianClass>
inline float LoadFloat(const char* p) {
  return absl::bit_cast<float>(EndianClass::ToHost32(UNALIGNED_LOAD32(p)));
}

template<typename EndianClass>
inline void StoreFloat(float value, char* p) {
  UNALIGNED_STORE32(p,
                    EndianClass::FromHost32(absl::bit_cast<uint32>(value)));
}

template<typename EndianClass>
inline double LoadDouble(const char* p) {
  return absl::bit_cast<double>(EndianClass::ToHost64(UNALIGNED_LOAD64(p)));
}

template<typename EndianClass>
inline void StoreDouble(double value, char* p) {
  UNALIGNED_STORE64(p,
                    EndianClass::FromHost64(absl::bit_cast<uint64>(value)));
}

}  // namespace endian_internal

// Load/Store for integral values.

template<typename T>
inline T LittleEndian::Load(const char* p) {
  return endian_internal::LoadInteger<LittleEndian, T>(p);
}

template<typename T>
inline void LittleEndian::Store(T value, char* p) {
  endian_internal::StoreInteger<LittleEndian, T>(value, p);
}

template<typename T>
inline T BigEndian::Load(const char* p) {
  return endian_internal::LoadInteger<BigEndian, T>(p);
}

template<typename T>
inline void BigEndian::Store(T value, char* p) {
  endian_internal::StoreInteger<BigEndian, T>(value, p);
}

// Load/Store for bool. Sanitizes bool on the way in for safety.

template<>
inline bool LittleEndian::Load<bool>(const char* p) {
  static_assert(sizeof(bool) == 1, "Unexpected sizeof(bool)");
  return *p != 0;
}

template<>
inline void LittleEndian::Store<bool>(bool value, char* p) {
  static_assert(sizeof(bool) == 1, "Unexpected sizeof(bool)");
  *p = value ? 1 : 0;
}

template<>
inline bool BigEndian::Load<bool>(const char* p) {
  static_assert(sizeof(bool) == 1, "Unexpected sizeof(bool)");
  return *p != 0;
}

template<>
inline void BigEndian::Store<bool>(bool value, char* p) {
  static_assert(sizeof(bool) == 1, "Unexpected sizeof(bool)");
  *p = value ? 1 : 0;
}

// Load/Store for float.

template<>
inline float LittleEndian::Load<float>(const char* p) {
  return endian_internal::LoadFloat<LittleEndian>(p);
}

template<>
inline void LittleEndian::Store<float>(float value, char* p) {
  endian_internal::StoreFloat<LittleEndian>(value, p);
}

template<>
inline float BigEndian::Load<float>(const char* p) {
  return endian_internal::LoadFloat<BigEndian>(p);
}

template<>
inline void BigEndian::Store<float>(float value, char* p) {
  endian_internal::StoreFloat<BigEndian>(value, p);
}

// Load/Store for double.

template<>
inline double LittleEndian::Load<double>(const char* p) {
  return endian_internal::LoadDouble<LittleEndian>(p);
}

template<>
inline void LittleEndian::Store<double>(double value, char* p) {
  endian_internal::StoreDouble<LittleEndian>(value, p);
}

template<>
inline double BigEndian::Load<double>(const char* p) {
  return endian_internal::LoadDouble<BigEndian>(p);
}

template<>
inline void BigEndian::Store<double>(double value, char* p) {
  endian_internal::StoreDouble<BigEndian>(value, p);
}

// Load/Store for uint128.

template <>
inline absl::uint128 LittleEndian::Load<absl::uint128>(const char* p) {
  return LittleEndian::Load128(p);
}

template <>
inline void LittleEndian::Store<absl::uint128>(absl::uint128 value, char* p) {
  LittleEndian::Store128(p, value);
}

template <>
inline absl::uint128 BigEndian::Load<absl::uint128>(const char* p) {
  return BigEndian::Load128(p);
}

template <>
inline void BigEndian::Store<absl::uint128>(absl::uint128 value, char* p) {
  BigEndian::Store128(p, value);
}

#endif  // S2_UTIL_ENDIAN_ENDIAN_H_
