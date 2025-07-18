// Copyright 2018 Google Inc. All Rights Reserved.
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

// Author: ericv@google.com (Eric Veach)

#ifndef S2_ENCODED_STRING_VECTOR_H_
#define S2_ENCODED_STRING_VECTOR_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "s2/util/coding/coder.h"
#include "s2/_fp_contract_off.h"  // IWYU pragma: keep
#include "s2/encoded_uint_vector.h"

namespace s2coding {

// This class allows an EncodedStringVector to be created by adding strings
// incrementally.  It also supports adding strings that are the output of
// another Encoder.  For example, to create a vector of encoded S2Polygons,
// you can do this:
//
// void EncodePolygons(const vector<S2Polygon*>& polygons, Encoder* encoder) {
//   StringVectorEncoder encoded_polygons;
//   for (auto polygon : polygons) {
//     polygon->Encode(encoded_polygons.AddViaEncoder());
//   }
//   encoded_polygons.Encode(encoder);
// }
class StringVectorEncoder {
 public:
  StringVectorEncoder();

  // Adds a string to the encoded vector.
  void Add(absl::string_view str);

  // Adds a string to the encoded vector by means of the given Encoder.  The
  // string consists of all output added to the encoder before the next call
  // to any method of this class (after which the encoder is no longer valid).
  Encoder* AddViaEncoder();

  // Appends the EncodedStringVector representation to the given Encoder.
  //
  // REQUIRES: "encoder" uses the default constructor, so that its buffer
  //           can be enlarged as necessary by calling Ensure(int).
  void Encode(Encoder* encoder);

  // Encodes a vector of strings in a format that can later be decoded as an
  // EncodedStringVector.
  //
  // REQUIRES: "encoder" uses the default constructor, so that its buffer
  //           can be enlarged as necessary by calling Ensure(int).
  static void Encode(absl::Span<const std::string> v, Encoder* encoder);

 private:
  // A vector consisting of the starting offset of each string in the
  // encoder's data buffer, plus a final entry pointing just past the end of
  // the last string.
  std::vector<uint64_t> offsets_;
  Encoder data_;
};

// This class represents an encoded vector of strings.  Values are decoded
// only when they are accessed.  This allows for very fast initialization and
// no additional memory use beyond the encoded data.  The encoded data is not
// owned by this class; typically it points into a large contiguous buffer
// that contains other encoded data as well.
//
// This is one of several helper classes that allow complex data structures to
// be initialized from an encoded format in constant time and then decoded on
// demand.  This can be a big performance advantage when only a small part of
// the data structure is actually used.
class EncodedStringVector {
 public:
  // Constructs an uninitialized object; requires Init() to be called.
  EncodedStringVector() = default;

  // Initializes the EncodedStringVector.  Returns false on errors, leaving
  // the vector in an unspecified state.
  //
  // REQUIRES: The Decoder data buffer must outlive this object.
  bool Init(Decoder* decoder);

  // Resets the vector to be empty.
  void Clear();

  // Returns the size of the original vector.
  size_t size() const;

  // Returns the string at the given index.
  absl::string_view operator[](size_t i) const;

  // Returns a Decoder initialized with the string at the given index.
  Decoder GetDecoder(size_t i) const;

  // Returns a pointer to the start of the string at the given index.  This is
  // faster than operator[] but returns an unbounded string.
  const char* GetStart(size_t i) const;

  // Returns the entire vector of original strings.  Requires that the
  // data buffer passed to the constructor persists until the result vector is
  // no longer needed.
  std::vector<absl::string_view> Decode() const;

  // Copies the encoded byte stream to a new encoder.
  void Encode(Encoder* encoder) const;

 private:
  EncodedUintVector<uint64_t> offsets_;
  const char* data_ = nullptr;
};


//////////////////   Implementation details follow   ////////////////////

inline void StringVectorEncoder::Add(absl::string_view str) {
  offsets_.push_back(data_.length());
  data_.Ensure(str.size());
  data_.putn(str.data(), str.size());
}

inline Encoder* StringVectorEncoder::AddViaEncoder() {
  offsets_.push_back(data_.length());
  return &data_;
}

inline void EncodedStringVector::Clear() {
  offsets_.Clear();
  data_ = nullptr;
}

inline size_t EncodedStringVector::size() const {
  return offsets_.size();
}

inline absl::string_view EncodedStringVector::operator[](size_t i) const {
  uint64_t start = (i == 0) ? 0 : offsets_[i - 1];
  uint64_t limit = offsets_[i];
  return absl::string_view(data_ + start, limit - start);
}

inline Decoder EncodedStringVector::GetDecoder(size_t i) const {
  // Return an empty decoder if we don't have enough data.
  if (i >= offsets_.size()) {
    return Decoder(nullptr, 0);
  }

  uint64_t start = (i == 0) ? 0 : offsets_[i - 1];
  uint64_t limit = offsets_[i];
  return Decoder(data_ + start, limit - start);
}

inline const char* EncodedStringVector::GetStart(size_t i) const {
  uint64_t start = (i == 0) ? 0 : offsets_[i - 1];
  return data_ + start;
}

}  // namespace s2coding

#endif  // S2_ENCODED_STRING_VECTOR_H_
