#include "ABC_Parser.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <cctype>
#include <algorithm>

ABCParser::ABCParser()
    : mValid(false)
{
    // I set sensible defaults here in case any header fields are missing
    mScore.bpm = 120.0;
    mScore.defaultNoteLength = 0.125; // I default to 1/8 as most abc files use this
    mScore.beatsPerBar = 4;
    mScore.beatUnit = 4;
    mScore.keyRoot = 'C';
}

bool ABCParser::loadFromFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        mLastError = "could not open file: " + filePath;
        return false;
    }
    // I read the whole file into a string and hand off to loadFromString
    // this keeps all the actual parsing logic in one place
    std::stringstream ss;
    ss << file.rdbuf();
    return loadFromString(ss.str());
}

bool ABCParser::loadFromString(const std::string& abcText) {
    // I reset everything so calling this twice gives a clean result
    mValid = false;
    mLastError.clear();
    mKeyAccidentals.clear();
    mScore = ABCScore{};
    mScore.bpm = 120.0;
    mScore.defaultNoteLength = 0.125;
    mScore.beatsPerBar = 4;
    mScore.beatUnit = 4;
    mScore.keyRoot = 'C';

    std::istringstream stream(abcText);
    std::string line;
    bool inHeader = true;

    // I collect each V: voice as its own body string so I can parse them
    // independently and then merge by time, giving true simultaneous playback.
    // voiceBodies[0] is used for any content before the first V: tag (single-voice files).
    std::vector<std::string> voiceBodies;
    voiceBodies.push_back(""); // slot 0: pre-voice or single-voice content
    std::map<std::string, int> voiceIndexById;
    int currentVoice = 0;

    auto getVoiceIndex = [&](const std::string& id) -> int {
        auto it = voiceIndexById.find(id);
        if (it != voiceIndexById.end())
            return it->second;

        voiceBodies.push_back("");
        int index = (int)voiceBodies.size() - 1;
        voiceIndexById[id] = index;
        return index;
    };

    while (std::getline(stream, line)) {
        // I strip carriage returns so windows line endings don't break anything
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty()) continue;
        if (line.rfind("%%", 0) == 0) continue;

        if (inHeader && line.size() >= 2 && line[1] == ':') {
            parseHeader(line);
            // I treat K: as the last header line as per the abc spec
            if (line[0] == 'K') inHeader = false;
        }
        else if (!inHeader) {
            if (line.size() >= 2 && line[1] == ':') {
                if (line[0] == 'V') {
                    // I only use the first bit after V: as the voice id.
                    // Stuff after that is usually name/clef metadata.
                    std::string rest = line.substr(2);
                    rest.erase(0, rest.find_first_not_of(" \t"));

                    size_t idEnd = rest.find_first_of(" \t");
                    std::string id = rest.substr(0, idEnd);
                    if (!id.empty())
                        currentVoice = getVoiceIndex(id);
                }
                // Other mid-tune fields (M:, Q: etc.) are ignored
                continue;
            }

            // I also handle inline voice labels like [V:V1] notes...
            if (line.rfind("[V:", 0) == 0) {
                size_t close = line.find(']');
                if (close != std::string::npos) {
                    std::string id = line.substr(3, close - 3);
                    currentVoice = getVoiceIndex(id);
                    voiceBodies[currentVoice] += line.substr(close + 1) + " ";
                    continue;
                }
            }

            voiceBodies[currentVoice] += line + " ";
        }
    }

    // If the file used V: blocks, merge them; otherwise fall back to the old single-voice path
    if (voiceBodies.size() > 1) {
        mergeVoices(voiceBodies);
    }
    else {
        parseTuneBody(voiceBodies[0]);
        buildFlatList();
    }

    mValid = !mScore.allNotes.empty();
    return mValid;
}

// -----------------------------------------------------------------------

void ABCParser::parseHeader(const std::string& line) {
    char field = line[0];
    std::string value = line.substr(2);
    // I trim leading whitespace from the value in case there's a space after the colon
    value.erase(0, value.find_first_not_of(" \t"));
    parseField(field, value);
}

void ABCParser::parseField(char field, const std::string& value) {
    // I dispatch each header field to its own parser
    // unrecognised fields are silently ignored
    switch (field) {
    case 'T': mScore.title = value;       break;
    case 'Q': parseTempo(value);          break;
    case 'M': parseTimeSignature(value);  break;
    case 'L': parseDefaultLength(value);  break;
    case 'K': parseKeySignature(value);   break;
    default:  break;
    }
}

void ABCParser::parseTimeSignature(const std::string& value) {
    // I handle the two common shorthand forms first before trying to split on /
    if (value == "C") { mScore.beatsPerBar = 4; mScore.beatUnit = 4; return; }
    if (value == "C|") { mScore.beatsPerBar = 2; mScore.beatUnit = 2; return; }

    auto slash = value.find('/');
    if (slash != std::string::npos) {
        mScore.beatsPerBar = std::stoi(value.substr(0, slash));
        mScore.beatUnit = std::stoi(value.substr(slash + 1));
    }
}

void ABCParser::parseDefaultLength(const std::string& value) {
    // I store this as a decimal fraction of a whole note, e.g. L:1/8 becomes 0.125
    auto slash = value.find('/');
    if (slash != std::string::npos) {
        int num = std::stoi(value.substr(0, slash));
        int denom = std::stoi(value.substr(slash + 1));
        mScore.defaultNoteLength = (double)num / (double)denom;
    }
}

void ABCParser::parseTempo(const std::string& value) {
    // I handle both plain "120" and the "1/4=120" form where a note value is specified
    // I only care about the bpm number, not the note value it's attached to
    auto eq = value.find('=');
    if (eq != std::string::npos)
        mScore.bpm = std::stod(value.substr(eq + 1));
    else
        mScore.bpm = std::stod(value);
}

void ABCParser::parseKeySignature(const std::string& value) {
    if (value.empty()) return;
    mScore.keyRoot = toupper(value[0]);

    // Major key signatures and their sharpened/flattened notes:
    static const std::map<std::string, std::map<char, int>> keySigs = {
        {"C",  {}},
        {"G",  {{'F',+1}}},
        {"D",  {{'F',+1},{'C',+1}}},
        {"A",  {{'F',+1},{'C',+1},{'G',+1}}},
        {"E",  {{'F',+1},{'C',+1},{'G',+1},{'D',+1}}},
        {"B",  {{'F',+1},{'C',+1},{'G',+1},{'D',+1},{'A',+1}}},
        {"F",  {{'B',-1}}},
        {"Bb", {{'B',-1},{'E',-1}}},
        // could add more as necessary
    };

    // Match on root + optional b for flats
    std::string key;
    key += toupper(value[0]);
    if (value.size() > 1 && value[1] == 'b') key += 'b';

    auto it = keySigs.find(key);
    if (it != keySigs.end())
        mKeyAccidentals = it->second;
    else
        mKeyAccidentals.clear();
}

// -----------------------------------------------------------------------

void ABCParser::parseTuneBody(const std::string& body) {
    // I split on | to get individual bars, then parse each one separately
    std::string current;
    for (char c : body) {
        if (c == '|') {
            if (!current.empty()) {
                ABCBar bar;
                parseBar(current, bar);
                if (!bar.notes.empty())
                    mScore.bars.push_back(bar);
                current.clear();
            }
        }
        else {
            current += c;
        }
    }
    // I handle the last bar separately since it may not have a trailing |
    if (!current.empty()) {
        ABCBar bar;
        parseBar(current, bar);
        if (!bar.notes.empty())
            mScore.bars.push_back(bar);
    }
}

// -----------------------------------------------------------------------
//  parseSingleNote
//  Shared helper used by both parseBar (for bare notes) and parseChord
//  (for notes inside [...]).  Reads one note token starting at position i
// inside text, advances i past it, and fills out a partially-constructed
//  ABCNote using the score's current defaultNoteLength and beatUnit.
//  Returns false if the character at i is not a valid note or rest.
// -----------------------------------------------------------------------
bool ABCParser::parseSingleNote(const std::string& text, size_t& i, ABCNote& note) const
{
    if (i >= text.size()) return false;

    char c = text[i];

    // Skip non-ASCII bytes (e.g. UTF-8 characters bleeding in from titles)
    if ((unsigned char)c > 127) { ++i; return false; }

    // I check for accidental prefixes before the note letter
    Accidental acc = Accidental::None;
    if (c == '^') {
        // Double sharp if two carets appear together
        if (i + 1 < text.size() && text[i + 1] == '^') { acc = Accidental::DoubleSharp; i += 2; }
        else { acc = Accidental::Sharp; ++i; }
        if (i >= text.size()) return false;
        c = text[i];
    }
    else if (c == '_') {
        // Double flat if two underscores appear together
        if (i + 1 < text.size() && text[i + 1] == '_') { acc = Accidental::DoubleFlat; i += 2; }
        else { acc = Accidental::Flat; ++i; }
        if (i >= text.size()) return false;
        c = text[i];
    }
    else if (c == '=') {
        acc = Accidental::Natural; ++i;
        if (i >= text.size()) return false;
        c = text[i];
    }

    // x/X are invisible rests (multi-voice spacers), treat same as z/Z
    bool isRest = (c == 'z' || c == 'Z' || c == 'x' || c == 'X');
    bool isNote = isRest || (toupper(c) >= 'A' && toupper(c) <= 'G');

    if (!isNote) return false;

    // I use lowercase as octave 1 and uppercase as octave 0 per the abc spec
    int  octave = std::islower(c) ? 1 : 0;
    char letter = (char)toupper(c);
    ++i;

    // I handle the ' and , octave shift characters that follow the note letter
    while (i < text.size() && (text[i] == '\'' || text[i] == ',')) {
        octave += (text[i] == '\'') ? 1 : -1;
        ++i;
    }

    // I collect the length modifier string, which may be digits, a slash, or both
    std::string lenMod;
    while (i < text.size() && (std::isdigit((unsigned char)text[i]) || text[i] == '/'))
        lenMod += text[i++];

    note.isRest = isRest;
    note.letter = isRest ? ' ' : letter;
    note.octave = octave;
    note.accidental = acc;
    note.gapBeats = 0.0;
    note.chordStart = true; // caller overrides this for notes after the first in a chord

    // I normalise duration to quarter-note beats so the audio callback never needs
    // to think about the default note length or time signature denominator
    note.durationBeats = mScore.defaultNoteLength
        * parseLengthMod(lenMod)
        * 4.0; // defaultNoteLength is a fraction of a whole note; x4 gives quarter-note beats

    if (acc == Accidental::None && !note.isRest) {
        auto it = mKeyAccidentals.find(note.letter);
        if (it != mKeyAccidentals.end())
            note.accidental = (it->second == +1) ? Accidental::Sharp : Accidental::Flat;
    }

    return true;
}

// -----------------------------------------------------------------------
// parseChord
//  Called when a '[' is encountered in the tune body (and it is NOT an
//  inline field like [V:1]).  Reads all notes between '[' and ']', tags
//  them with chordStart correctly, and appends them to bar.
//  i should point at the character AFTER the opening '[' on entry; on
//  return i points at the character after the closing ']'.
// -----------------------------------------------------------------------
void ABCParser::parseChord(const std::string& text, size_t& i, ABCBar& bar) const
{
    std::vector<ABCNote> chordNotes;

    while (i < text.size() && text[i] != ']') {
        // Skip whitespace and non-note decorations inside a chord
        char c = text[i];
        if (c == ' ' || c == '-' || c == '~') { ++i; continue; }
        if ((unsigned char)c > 127) { ++i; continue; }

        ABCNote note;
        if (parseSingleNote(text, i, note))
            chordNotes.push_back(note);
        else
            ++i; // skip unrecognised character inside chord
    }

    if (i < text.size()) ++i; // skip the closing ']'

    if (chordNotes.empty()) return;

    // The first note in the chord advances the sequencer clock (chordStart = true).
    // All subsequent notes share the same start time (chordStart = false) and
    // inherit the first note's duration so they all end together.
    chordNotes[0].chordStart = true;
    double chordDuration = chordNotes[0].durationBeats;

    for (size_t n = 1; n < chordNotes.size(); ++n) {
        chordNotes[n].chordStart = false;
        chordNotes[n].durationBeats = chordDuration; // unify duration across the chord
    }

    for (auto& cn : chordNotes)
        bar.notes.push_back(cn);
}

// -----------------------------------------------------------------------

void ABCParser::parseBar(const std::string& barText, ABCBar& bar) {
    size_t i = 0;
    while (i < barText.size()) {
        char c = barText[i];

        if (c == ' ') { ++i; continue; }

        // Skip non-ASCII bytes
        if ((unsigned char)c > 127) { ++i; continue; }

        // Skip tie/slur markers and broken rhythm operators
        if (c == '-' || c == '>' || c == '<' || c == '(' || c == ')') { ++i; continue; }

        // Handle [...] -- could be a chord or an inline field like [V:1]
        if (c == '[') {
            ++i; // move past '['

            // Detect inline fields: a letter followed immediately by ':'
            // e.g. [V:1], [M:3/4]. These are not chords; skip them entirely.
            if (i + 1 < barText.size() && std::isalpha((unsigned char)barText[i]) && barText[i + 1] == ':') {
                while (i < barText.size() && barText[i] != ']') ++i;
                if (i < barText.size()) ++i; // skip ']'
                continue;
            }

            // Otherwise treat as a chord and parse all notes inside it
            parseChord(barText, i, bar);
            continue;
        }

        // Skip note groups {/B} or {B} (grace notes)
        if (c == '{') {
            while (i < barText.size() && barText[i] != '}') ++i;
            if (i < barText.size()) ++i;
            continue;
        }

        // Try to parse a bare (non-chord) note at this position
        ABCNote note;
        if (parseSingleNote(barText, i, note)) {
            note.chordStart = true; // bare notes always start a new time position
            bar.notes.push_back(note);
        }
        else {
            ++i; // skip unrecognised character
        }
    }
}

// -----------------------------------------------------------------------

double ABCParser::parseLengthMod(const std::string& mod) const {
    // I return 1.0 when there's no modifier so the default note length is used unchanged
    if (mod.empty()) return 1.0;

    // I handle the shorthand slash forms that abc uses for halving
    if (mod == "/")  return 0.5;
    if (mod == "//") return 0.25;

    auto slash = mod.find('/');
    if (slash == std::string::npos) {
        if (!std::isdigit((unsigned char)mod[0])) return 1.0;
        return std::stod(mod);
    }

    // n/m form -- either side may be absent, defaulting to num=1, denom=2
    std::string numStr = mod.substr(0, slash);
    std::string denomStr = mod.substr(slash + 1);

    double num = (!numStr.empty() && std::isdigit((unsigned char)numStr[0])) ? std::stod(numStr) : 1.0;
    double denom = (!denomStr.empty() && std::isdigit((unsigned char)denomStr[0])) ? std::stod(denomStr) : 2.0;

    return (denom != 0.0) ? num / denom : 1.0;
}

double ABCParser::letterToFreq(char letter, int octave, Accidental acc) const {
    // I calculate frequency using A4 = 440hz as the reference
    static const int semitones[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G

    int semi = semitones[letter - 'A'];

    switch (acc) {
    case Accidental::Sharp:       semi += 1; break;
    case Accidental::Flat:        semi -= 1; break;
    case Accidental::DoubleSharp: semi += 2; break;
    case Accidental::DoubleFlat:  semi -= 2; break;
    default: break;
    }

    int semiFromA4 = semi - 9 + ((octave - 1) * 12); // I take away 1 because otherwise its too high pitch
    return 440.0 * std::pow(2.0, semiFromA4 / 12.0);
}

// -----------------------------------------------------------------------
// mergeVoices
//  Takes one body string per voice, parses each independently into its own
//  note list with its own time cursor, then merges all voices into allNotes
//  sorted by start time.  Notes that share a start time are chained with
//  chordStart = false so the sequencer fires them simultaneously.
// -----------------------------------------------------------------------
void ABCParser::mergeVoices(const std::vector<std::string>& voiceBodies)
{
    // I use a flat list of (startBeat, note) pairs so I can sort by time after
    // parsing all voices independently
    struct TimedNote {
        double   startBeat;
        ABCNote  note;
    };

    std::vector<TimedNote> allTimed;

    for (const auto& body : voiceBodies) {
        if (body.empty()) continue;

        // Parse this voice into its own temporary bar list by calling the
        // existing parseTuneBody/buildFlatList machinery on a clean temporary score.
        // I save and restore mScore so the header data stays intact.
        ABCScore savedScore = mScore;
        mScore.bars.clear();
        mScore.allNotes.clear();

        parseTuneBody(body);
        buildFlatList();

        // Walk the flat note list and assign absolute start times.
        // chordStart == false means "same time as the previous note", so I only
        // advance the cursor for notes that start a new time position.
        double cursor = 0.0;
        for (const auto& note : mScore.allNotes) {
            if (note.chordStart) {
                allTimed.push_back({ cursor, note });
                cursor += note.durationBeats + note.gapBeats;
            }
            else {
                // Chord continuation -- same start time as the last chordStart note
                allTimed.push_back({ allTimed.back().startBeat, note });
            }
        }

        // Restore the score header fields (title, bpm, key etc.) -- only bars/notes changed
        mScore = savedScore;
    }

    // Sort all timed notes from every voice by start time.
    // std::stable_sort keeps equal-time notes in voice order, which is fine.
    std::stable_sort(allTimed.begin(), allTimed.end(),
        [](const TimedNote& a, const TimedNote& b) {
            return a.startBeat < b.startBeat;
        });

    // I rebuild allNotes as time groups here.  The first note at each time
    // position advances the sequencer to the next time position, while the
    // other notes at that time behave like chord notes.
    mScore.allNotes.clear();
    mScore.bars.clear();

    size_t i = 0;
    while (i < allTimed.size()) {
        double groupStart = allTimed[i].startBeat;
        size_t groupEnd = i + 1;

        while (groupEnd < allTimed.size()
            && std::fabs(allTimed[groupEnd].startBeat - groupStart) < 1e-9) {
            ++groupEnd;
        }

        double nextStart = groupStart;
        if (groupEnd < allTimed.size())
            nextStart = allTimed[groupEnd].startBeat;

        for (size_t nIndex = i; nIndex < groupEnd; ++nIndex) {
            ABCNote n = allTimed[nIndex].note;
            n.chordStart = (nIndex == i);

            if (n.chordStart && groupEnd < allTimed.size())
                n.gapBeats = (nextStart - groupStart) - n.durationBeats;

            mScore.allNotes.push_back(n);
        }

        i = groupEnd;
    }
}

void ABCParser::buildFlatList() {
    // I flatten the bar/note hierarchy into a single array here
    // this is the only array the audio callback ever needs to touch
    mScore.allNotes.clear();
    for (auto& bar : mScore.bars)
        for (auto& note : bar.notes)
            mScore.allNotes.push_back(note);
}

// -----------------------------------------------------------------------

const ABCScore& ABCParser::getScore()     const { return mScore; }
bool            ABCParser::isValid()      const { return mValid; }
std::string     ABCParser::getLastError() const { return mLastError; }
