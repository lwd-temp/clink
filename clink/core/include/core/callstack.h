// Copyright (c) 2022 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#ifdef DEBUG

#define MAX_ADDRESS_LEN         (2 + 16)
#define MAX_MODULE_LEN          (24)
#define MAX_SYMBOL_LEN          (128)

//                      "\t" or " "      MODULE      "! "     SYMBOL     " + "   0xOFFSET       "\r\n" or " /"
#define MAX_FRAME_LEN           (1 + MAX_MODULE_LEN + 2 + MAX_SYMBOL_LEN + 3 + MAX_ADDRESS_LEN + 2)

#define DEFAULT_CALLSTACK_LEN   (MAX_FRAME_LEN * 20 + 1)

#ifdef __cplusplus
#define CALLSTACK_EXTERN_C      extern "C"
#endif

// Formats buffer (capacity is size of buffer) with up to total_frames, skipping
// the first skip_frames.  The frames are delimited with newlines.
CALLSTACK_EXTERN_C size_t format_callstack(int skip_frames, int total_frames, char* buffer, size_t capacity);

// Copies stack frame pointers.  They can can formatted later with
// format_frames().
CALLSTACK_EXTERN_C int get_callstack_frames(int skip_frames, int total_frames, void** frames);

// Formats buffer (capacity is size of buffer) with up to total_frames.  The
// frames are delimited with slashes or newlines.
CALLSTACK_EXTERN_C size_t format_frames(int total_frames, void* const* frames, char* buffer, size_t capacity, int newlines);

#endif // DEBUG