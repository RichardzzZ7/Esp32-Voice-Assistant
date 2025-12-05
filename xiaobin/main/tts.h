// tts.h - TTS interface (supports caching). Simple local fallback implemented.
#ifndef _TTS_H_
#define _TTS_H_

#include <stdbool.h>

void tts_init(void);
// Speak text (blocking). If 'cache' true, save synthesized audio in cache.
int tts_speak_text(const char *text, bool cache);

#endif // _TTS_H_
