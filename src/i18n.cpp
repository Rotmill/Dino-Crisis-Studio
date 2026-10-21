#include "i18n.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dc {
int g_lang = LANG_EN;

int system_lang() {
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_RUSSIAN || PRIMARYLANGID(id) == LANG_UKRAINIAN || PRIMARYLANGID(id) == LANG_BELARUSIAN) return LANG_RU;
#endif
    return LANG_EN;
}
}  // namespace dc
