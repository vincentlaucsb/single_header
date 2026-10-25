#pragma once

#include <llvm/Support/Compiler.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/AllocatorBase.h>
#include <llvm/Support/MathExtras.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>

// LLVM 20's arena allocator changes its inline definitions when the *client*
// enables ASan. Mixing those definitions with an unsanitized SDK can poison a
// slab in the client and then allocate from it in SDK code without unpoisoning.
// Match only this header's manual arena annotations to that SDK. Compiler ASan
// instrumentation remains enabled for our code, including these inline methods;
// heap redzones, use-after-free checks and UBSan remain active.
#if defined(SINGLE_HEADER_CLANG_UNSANITIZED_SDK) && LLVM_ADDRESS_SANITIZER_BUILD
#pragma push_macro("LLVM_ADDRESS_SANITIZER_BUILD")
#pragma push_macro("__asan_poison_memory_region")
#pragma push_macro("__asan_unpoison_memory_region")
#undef LLVM_ADDRESS_SANITIZER_BUILD
#define LLVM_ADDRESS_SANITIZER_BUILD 0
#undef __asan_poison_memory_region
#define __asan_poison_memory_region(address, size) ((void)0)
#undef __asan_unpoison_memory_region
#define __asan_unpoison_memory_region(address, size) ((void)0)
#include <llvm/Support/Allocator.h>
#pragma pop_macro("__asan_unpoison_memory_region")
#pragma pop_macro("__asan_poison_memory_region")
#pragma pop_macro("LLVM_ADDRESS_SANITIZER_BUILD")
#else
#include <llvm/Support/Allocator.h>
#endif
