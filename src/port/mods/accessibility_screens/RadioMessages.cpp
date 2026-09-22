#include "AccessibilityScreens.h"

#include <cstring>
#include <string>

#include "port/CGameCompat.h"
#include "port/mods/Accessibility.h"
#include "port/accessibility/Tts.h"
#include "port/hooks/Events.h"

// Speaks radio messages that have no voice acting — the Training tips shown as
// text only. The game marks that set itself: fox_radio.c drives the mouth/beep
// animation from the printed text (instead of the voice stream) exactly for
// message IDs 23000-23032, so the same range gates the speech here. Voiced
// messages are never spoken; their voice acting already carries the content.
static bool Accessibility_IsTextOnlyMessage(s32 msgId) {
    return (msgId >= 23000) && (msgId < 23033);
}

// Dedicated toggle (default on): the tips repeat on a timer every lap of the
// Training script, so players who know them can silence this without
// disabling the reader.
static bool Accessibility_IsRadioTextEnabled() {
    return CVarGetInteger("gAccessibilityRadioText", 1) == 1;
}

static bool Accessibility_EndsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Decodes a MSGCHAR u16 stream into speakable text. Beyond the 1:1 letter /
// digit / punctuation mapping, the glyph-level quirks are smoothed for speech:
// on screen a button reference renders as the letter C + an arrow glyph +
// the redundant yellow C-button icon ("Press C▼ [icon] to brake"), so an arrow
// joins a directly preceding C as "C-down" and an icon that merely repeats the
// phrase just produced is dropped — an icon appearing alone still speaks as
// "C-down". Newlines are layout, not semantics, and become spaces.
static std::string Accessibility_DecodeMessage(const u16* chars) {
    static const char* sArrowWords[] = { "up", "left", "down", "right" };           // AUP ALF ADN ART
    static const char* sCButtonWords[] = { "C-left", "C-up", "C-right", "C-down" }; // CLF CUP CRT CDN
    std::string out;

    auto appendSpace = [&out]() {
        if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
    };
    auto trimTrailingSpace = [&out]() {
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
    };

    for (size_t i = 0; chars[i] != MSGCHAR_END; i++) {
        u16 c = chars[i];
        if (c >= MSGCHAR_A && c <= MSGCHAR_Z) {
            out.push_back((char) ('A' + (c - MSGCHAR_A)));
        } else if (c >= MSGCHAR_a && c <= MSGCHAR_z) {
            out.push_back((char) ('a' + (c - MSGCHAR_a)));
        } else if (c >= MSGCHAR_0 && c <= MSGCHAR_9) {
            out.push_back((char) ('0' + (c - MSGCHAR_0)));
        } else if (c >= MSGCHAR_AUP && c <= MSGCHAR_ART) {
            if (!out.empty() && out.back() == 'C') {
                out.push_back('-'); // the textual "C▼" notation
            } else {
                appendSpace();
            }
            out += sArrowWords[c - MSGCHAR_AUP];
        } else if (c >= MSGCHAR_CLF && c <= MSGCHAR_CDN) {
            const char* word = sCButtonWords[c - MSGCHAR_CLF];
            trimTrailingSpace();
            if (!Accessibility_EndsWith(out, word)) {
                appendSpace();
                out += word;
            }
        } else {
            switch (c) {
                case MSGCHAR_NWL:
                case MSGCHAR_SPC:
                    appendSpace();
                    break;
                case MSGCHAR_NXT:
                    // Sentence break between text boxes; most boxes already end
                    // in punctuation, so only add a period when one is missing.
                    trimTrailingSpace();
                    if (!out.empty() && strchr(".!?", out.back()) == NULL) {
                        out.push_back('.');
                    }
                    out.push_back(' ');
                    break;
                case MSGCHAR_EXM:
                case MSGCHAR_QST:
                case MSGCHAR_CMA:
                case MSGCHAR_PRD:
                case MSGCHAR_CLN:
                case MSGCHAR_RPR: {
                    static const char sPunct[] = { '!', '?', ',', '.', ':', ')' };
                    static const u16 sPunctCode[] = { MSGCHAR_EXM, MSGCHAR_QST, MSGCHAR_CMA,
                                                      MSGCHAR_PRD, MSGCHAR_CLN, MSGCHAR_RPR };
                    trimTrailingSpace(); // punctuation attaches to the word before it
                    for (s32 p = 0; p < ARRAY_COUNT(sPunctCode); p++) {
                        if (sPunctCode[p] == c) {
                            out.push_back(sPunct[p]);
                            break;
                        }
                    }
                    break;
                }
                case MSGCHAR_DSH:
                    out.push_back('-');
                    break;
                case MSGCHAR_APS:
                    out.push_back('\'');
                    break;
                case MSGCHAR_LPR:
                    appendSpace();
                    out.push_back('(');
                    break;
                default:
                    // Layout codes (PRI*, NP*, QSP, HSP) and glyphs the US text
                    // never uses (accented letters) carry nothing to speak.
                    break;
            }
        }
    }
    trimTrailingSpace();
    return out;
}

static void Accessibility_OnRadioMessage(IEvent* event) {
    if (!Accessibility_IsScreenReaderEnabled() || !Accessibility_IsRadioTextEnabled()) {
        return;
    }
    RadioMessageEvent* e = (RadioMessageEvent*) event;
    if (!Accessibility_IsTextOnlyMessage(e->msgId)) {
        return;
    }
    const u16* chars = (const u16*) LOAD_ASSET(e->msg);
    if (chars == NULL) {
        return;
    }
    std::string text = Accessibility_DecodeMessage(chars);
    if (text.empty()) {
        return;
    }
    // Queue rather than interrupt: the tips arrive several seconds apart and
    // answer no button press, so they should not cut off in-flight speech.
    Tts_Speak(text.c_str(), false);
}

void AccessibilityRadioMessages_Register() {
    CVarRegisterInteger("gAccessibilityRadioText", 1);
    REGISTER_LISTENER(RadioMessageEvent, Accessibility_OnRadioMessage, EVENT_PRIORITY_NORMAL);
}
