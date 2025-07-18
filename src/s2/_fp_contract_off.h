// Copyright 2015 Google Inc. All Rights Reserved.
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

#ifndef S2__FP_CONTRACT_OFF_H_
#define S2__FP_CONTRACT_OFF_H_

// Turn off the fused multiply-add optimization ("fp-contract").  With
// fp-contract on, any expression of the form "a * b + c" has two possible
// results, and the compiler is free to choose either of them.  Effectively
// this makes it impossible to write deterministic functions that involve
// floating-point math.
//
// S2 requires deterministic arithmetic for correctness.  We need to turn off
// fp-contract for the entire compilation unit, because S2 has public inline
// functions, and the optimization is controlled by the setting in effect when
// inline functions are instantiated (not when they are defined).
//
// Note that there is a standard C pragma to turn off FP contraction:
//   #pragma STDC FP_CONTRACT OFF
// but it is not implemented in GCC because the standard pragma allows control
// at the level of compound statements rather than entire functions.
//
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=20785
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=37845
//
// This file may be included with other files in any order, as long as it
// appears before the first non-inline function definition.  It is
// named with an underscore so that it is included first among the S2 headers.

// Assume all compilers support or ignore the standard pragma.
#pragma STDC FP_CONTRACT OFF

#if defined(__GNUC__) && !defined(__clang__)
// GCC defines its own pragma that operates at the function level rather than
// the statement level.
#pragma GCC optimize("fp-contract=off")
#endif

#endif  // S2__FP_CONTRACT_OFF_H_
