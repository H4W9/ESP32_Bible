// BibleKeyboard.h
// Touch keyboard for MarauderBible search input.
// Standalone — no dependency on Display class or global objects.
//
// Occupies the bottom half of the screen.
// German characters are returned as single private bytes (same codes used in
// verse storage so the search can compare them directly):
//   0x80=Ä  0x81=ä  0x82=Ö  0x83=ö  0x84=Ü  0x85=ü  0x86=ß
//
// Layout (5 rows × 10 columns):
//   Row 0: 1 2 3 4 5 6 7 8 9 0
//   Row 1: q w e r t y u i o p
//   Row 2: a s d f g h j k l         (centered, 9 keys)
//   Row 3: z x c v b n m .    [CAPS CAPS]
//   Row 4: [X][SYM][ä][ö][ü][ß][SPC SPC][<X][OK]

#pragma once
#include "configs.h"

#ifdef HAS_TOUCH

#include <TFT_eSPI.h>
#include <stddef.h>

/**
 * Blocking touch-keyboard input dialog.
 *
 * @param tft           TFT_eSPI display to render on.
 * @param fg            Foreground (text) colour.
 * @param bg            Background colour.
 * @param buffer        Caller-allocated buffer.  On entry may contain a pre-fill
 *                      string; on exit contains the typed text (null-terminated).
 * @param bufLen        Total size of buffer including null terminator.
 * @param title         Optional prompt shown above the input box (may be nullptr).
 * @param partial_match Optional in/out: if non-null, draws a "Partial Match"
 *                      checkbox; when true each query word is matched independently
 *                      (any order/position) rather than as an exact phrase.
 * @param ignore_punct  Optional in/out: if non-null, draws an "Ignore Punctuation"
 *                      checkbox; value is toggled by the user.
 * @param scope         Optional in/out: if non-null, draws a "Scope" row with
 *                      Bible / Section / Book buttons; 0=Bible, 1=Section, 2=Book.
 *
 * @return true if the user pressed OK, false if they pressed Cancel.
 */
bool bibleKeyboardInput(TFT_eSPI& tft,
                        uint16_t  fg,
                        uint16_t  bg,
                        char*     buffer,
                        size_t    bufLen,
                        const char* title         = nullptr,
                        bool*       partial_match  = nullptr,
                        bool*       ignore_punct   = nullptr,
                        uint8_t*    scope          = nullptr);

#endif // HAS_TOUCH
