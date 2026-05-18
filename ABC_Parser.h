#pragma once
#include "ABC_Note.h"
#include <string>
#include <map>

class ABCParser {
public:
    ABCParser();

    // I provide two ways to load abc content, from a file path or directly from a string
    // loading from a string is useful for testing without needing files on disk
    bool        loadFromFile(const std::string& filePath);
    bool        loadFromString(const std::string& abcText);

    // I return the completed score after a successful parse
    const ABCScore& getScore() const;

    // I use these to let the caller check if parsing succeeded and why it failed
    bool        isValid()       const;
    std::string getLastError()  const;

private:
    // I split header parsing into per-field helpers to keep each one small and focused
    void parseHeader(const std::string& line);
    void parseField(char field, const std::string& value);
    void parseKeySignature(const std::string& value);
    void parseTimeSignature(const std::string& value);
    void parseDefaultLength(const std::string& value);
    void parseTempo(const std::string& value);

    // I split the tune body into bars first, then parse each bar into individual notes
    void parseTuneBody(const std::string& body);
    void parseBar(const std::string& barText, ABCBar& bar);

    // parseSingleNote reads one note token at position i in text, advances i past it,
    // and fills note. Returns false if no valid note was found at that position.
    bool parseSingleNote(const std::string& text, size_t& i, ABCNote& note) const;

    // parseChord is called after a '[' is found that is not an inline field.
    // It reads alln otes up to the matching ']' and appends them to bar with
    // correct chordStart flags set.
    void parseChord(const std::string& text, size_t& i, ABCBar& bar) const;

    // I use these helpers to convert raw abc tokens into usable values
    double parseLengthMod(const std::string& mod) const;
    double letterToFreq(char letter, int octave, Accidental acc) const;

    // I call this once at the end of parsing to flatten bars into the allNotes array
    void buildFlatList();

    // I use this to parse multiple V: voice blocks independently and then
    // interleave their notes by start time so they play simultaneously
    void mergeVoices(const std::vector<std::string>& voiceBodies);

    std::map<char, int> mKeyAccidentals; //{'F':+1, 'C':+1, 'G':+1, 'D':+1} for majors

    ABCScore    mScore;
    bool        mValid;
    std::string mLastError;
};