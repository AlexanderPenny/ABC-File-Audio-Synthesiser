#pragma once
#include <vector>
#include <string>

// I use this to represent the different accidental states a note can have
// none is the default when no accidental symbol appears before the note
enum class Accidental { None, Sharp, Flat, Natural, DoubleSharp, DoubleFlat };

// I keep this as a plain struct with no methods intentionally
// the audio callback needs to read this and I don't want any logic bleeding in here
struct ABCNote {
    char        letter;         // the note letter as it appears in the abc file, C D E F G A B
    int         octave;         // I track octave as an integer offset, 0 is the middle octave (uppercase in abc)
    // positive numbers go up, negative go down
    // lowercase abc notes are octave 1, a ' adds 1, a , subtracts 1
    Accidental  accidental;     // the accidental prefix if any was present before the note letter
    double      durationBeats;  // how long the note lasts in beats, where 1.0 is one quarter note
    double      gapBeats;       // silence after the note ends, I use this for staccato or explicit rests
    bool        isRest;         // true if this is a z or Z token rather than a pitched note

    // I use this to signal simultaneous notes (chords) to the sequencer.
    // The first note of a chord has chordStart = true; subsequent notes in the same
    // chord have chordStart = false. The sequencer fires all consecutive non-chordStart
    // notes at the same sample time as the note that preceded them, rather than
    // advancing the cursor. Only the first note's durationBeats is used to advance time.
    bool        chordStart;     // true if this note starts a new time position (default for all non-chord notes)
};

// I group notes into bars here because it helps me validate timing during parsing
// the audio callback won't use this directly, it uses allNotes in ABCScore instead
struct ABCBar {
    std::vector<ABCNote> notes;
};

// I store everything the audio callback needs to know about the tune in here
// once parsing is done this struct is effectively read-only from the audio side
struct ABCScore {
    std::string title;
    int         beatsPerBar;        // the numerator of the time signature, e.g. 4 in 4/4
    int         beatUnit;           // the denominator of the time signature, e.g. 4 in 4/4
    double      bpm;                // tempo in beats per minute, parsed from the Q: field
    double      defaultNoteLength;  // the base note length from L:, stored as a fraction of a whole note
    char        keyRoot;            // the root letter of the key signature from the K: field

    // I keep the bar structure here for validation and potential future use
    std::vector<ABCBar> bars;

    // I flatten all notes into this single array at the end of parsing
    // the audio callback only ever reads from here, just incrementing an index
    std::vector<ABCNote> allNotes;
};