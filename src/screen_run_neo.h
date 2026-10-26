#ifndef SCREEN_RUN_NEO_H
#define SCREEN_RUN_NEO_H

// Consume identical single-cell characters within the current row. The caller
// writes the final cell itself so wrapping and wide-character tail cleanup
// still follow the normal screen insertion path.
static inline int
screen_run_neo (const wchar_t *str, const wchar_t *end, unicode_t c,
                text_t *text, rend_t *rend, rend_t style, int available)
{
  int count = 1;
  while (count < available && str < end && unicode_t (*str) == c)
    {
      ++count;
      ++str;
    }

  // Separate fills let the compiler vectorize both screen buffers.
  for (int i = 0; i < count - 1; ++i)
    text[i] = c;
  for (int i = 0; i < count - 1; ++i)
    rend[i] = style;

  return count - 1;
}

#endif
