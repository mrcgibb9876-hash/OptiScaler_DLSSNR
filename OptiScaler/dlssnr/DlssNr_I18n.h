#pragma once

#include <string>

struct ImFontAtlas;

// The DLSS 5 panel's language. gettext-style, mirroring OptiDLSS5-UI's i18n.js: the English text
// is the key, Tr() hands back the translation for the active language or the English itself when
// there is none, so every string in DlssNr_Menu.cpp still reads as English at the call site.
//
// The tables live in DlssNr_I18n_Tables.cpp, generated from the manager repo's translation files
// (one flat English -> translation list per language, in the same order as kKeys). Format
// strings keep their printf specifiers verbatim; the generator checks that.
namespace DlssNr::I18n
{
// The translation of an English panel string for the active language, or the string itself.
// Stable pointers into static tables, safe to keep for the frame.
const char* Tr(const char* en);

// Resolves the active language from the [DlssNr] Language value: "auto" (or empty) follows the
// Windows display language, anything else is matched against the shipped codes, and an unknown
// code means English. Cheap when the value has not changed; RenderMenu calls it every frame.
void Refresh(const std::string& configured);

// The active language's code in the form the manager uses ("en", "pt-BR", "zh-CN", ...).
const char* ActiveCode();

// The panel's Language combo: index 0 is Auto, then the languages in the manager's order.
int SelectorIndex(const char* configured);
const char* CodeForSelectorIndex(int index);

// Merges the Windows font a language needs (Chinese, Korean) into the atlas, once. The base font
// (Hack) already covers Latin and Cyrillic. Safe to call every frame; it returns at once when
// there is nothing to do. Must run before ImGui::NewFrame(), which is when fonts may be added.
void EnsureFonts(ImFontAtlas* atlas, float fontSize);

// Generated tables.
extern const char* const kKeys[];
extern const int kKeyCount;
extern const char* const kLanguageCodes[];
extern const int kLanguageCount;
extern const char* const* const kLanguageTables[];
} // namespace DlssNr::I18n
