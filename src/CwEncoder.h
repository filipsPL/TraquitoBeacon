#pragma once

#include <cstdint>
#include <string>
#include <vector>
using namespace std;


// CW (Morse code) encoder.
//
// Converts a text string into a stream of (key_on, duration_ms) events using
// PARIS-standard timing (dit unit = 1200 / WPM milliseconds).
//
// Standard amateur-radio CW timing:
//   dit          = 1 unit, key on
//   dah          = 3 units, key on
//   intra-char   = 1 unit, key off (between elements of the same character)
//   inter-char   = 3 units, key off (between characters of the same word)
//   inter-word   = 7 units, key off (between words)
//
// Header-only and side-effect-free so the algorithm is testable from the host
// build and reusable independent of the transmitter.

class CwEncoder
{
public:

    struct Event
    {
        bool     keyOn;        // true = carrier on, false = carrier off
        uint32_t durationMs;
    };

    // Returns dit duration in milliseconds for the given WPM.
    // PARIS standard: 50 dit-units per word, so 1 dit = 60_000 / (50 * WPM) = 1200/WPM ms.
    static uint32_t DitMs(uint8_t wpm)
    {
        if (wpm == 0) { wpm = 18; }
        return 1200u / wpm;
    }

    // Estimate total transmission duration in milliseconds for a given text + WPM.
    // Convenient for the scheduler to know how long a CW slot will run.
    static uint32_t EstimateDurationMs(const string &text, uint8_t wpm)
    {
        uint32_t dit = DitMs(wpm);
        uint32_t units = 0;
        bool prevCharWasSpace = true;  // suppress leading inter-char gap

        for (char c : text)
        {
            const char *code = MorseFor(c);
            if (code == nullptr) { continue; }  // unknown chars dropped

            if (code[0] == ' ')  // word space
            {
                if (!prevCharWasSpace) { units += 7; }
                prevCharWasSpace = true;
                continue;
            }

            // inter-character gap before this character (if not the first)
            if (!prevCharWasSpace) { units += 3; }
            prevCharWasSpace = false;

            // count elements + intra-char gaps
            bool firstElement = true;
            for (const char *p = code; *p; ++p)
            {
                if (!firstElement) { units += 1; }
                units += (*p == '.') ? 1 : 3;
                firstElement = false;
            }
        }

        return units * dit;
    }

    // Generate the full key-on/key-off event stream for the message.
    // The output starts with a key-on for the first element (no leading silence)
    // and ends with the final element's key-on (no trailing silence).
    static vector<Event> Encode(const string &text, uint8_t wpm)
    {
        vector<Event> out;
        uint32_t dit = DitMs(wpm);
        bool prevCharWasSpace = true;

        for (char c : text)
        {
            const char *code = MorseFor(c);
            if (code == nullptr) { continue; }

            if (code[0] == ' ')
            {
                if (!prevCharWasSpace)
                {
                    out.push_back({ false, 7u * dit });
                }
                prevCharWasSpace = true;
                continue;
            }

            if (!prevCharWasSpace)
            {
                out.push_back({ false, 3u * dit });
            }
            prevCharWasSpace = false;

            bool firstElement = true;
            for (const char *p = code; *p; ++p)
            {
                if (!firstElement)
                {
                    out.push_back({ false, 1u * dit });
                }
                uint32_t lenUnits = (*p == '.') ? 1u : 3u;
                out.push_back({ true, lenUnits * dit });
                firstElement = false;
            }
        }

        return out;
    }


private:

    // Returns Morse code string for the character (uppercase letters, digits,
    // '/', '?', '.', ',', '='), or " " for word space, or nullptr if unknown.
    static const char *MorseFor(char c)
    {
        // Normalize to uppercase
        if (c >= 'a' && c <= 'z') { c = (char)(c - 'a' + 'A'); }

        switch (c)
        {
            case 'A': return ".-";
            case 'B': return "-...";
            case 'C': return "-.-.";
            case 'D': return "-..";
            case 'E': return ".";
            case 'F': return "..-.";
            case 'G': return "--.";
            case 'H': return "....";
            case 'I': return "..";
            case 'J': return ".---";
            case 'K': return "-.-";
            case 'L': return ".-..";
            case 'M': return "--";
            case 'N': return "-.";
            case 'O': return "---";
            case 'P': return ".--.";
            case 'Q': return "--.-";
            case 'R': return ".-.";
            case 'S': return "...";
            case 'T': return "-";
            case 'U': return "..-";
            case 'V': return "...-";
            case 'W': return ".--";
            case 'X': return "-..-";
            case 'Y': return "-.--";
            case 'Z': return "--..";
            case '0': return "-----";
            case '1': return ".----";
            case '2': return "..---";
            case '3': return "...--";
            case '4': return "....-";
            case '5': return ".....";
            case '6': return "-....";
            case '7': return "--...";
            case '8': return "---..";
            case '9': return "----.";
            case '/': return "-..-.";
            case '?': return "..--..";
            case '.': return ".-.-.-";
            case ',': return "--..--";
            case '=': return "-...-";
            case ' ': return " ";  // word separator sentinel
            default:  return nullptr;
        }
    }
};
