// Язык интерфейса: английский / русский. T("English", "Русский") — строка на текущем языке.
#pragma once

namespace dc {
enum Lang { LANG_EN = 0, LANG_RU = 1 };
extern int g_lang;
inline const char* T(const char* en, const char* ru) { return g_lang == LANG_RU ? ru : en; }
int system_lang();
}  // namespace dc
