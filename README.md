## Code Overview

This application acts as a custom digital audio synthesiser that translates alphanumeric **ABC notation** data into sound.

The code is structured to cleanly handle:
* **Text Parsing:** It reads through standard ABC notation format strings to extract musical keys, notes, pitches, accidentals, lengths, and tempo markings.
* **Waveform Synthesis:** It dynamically translates those parsed musical elements into their corresponding digital audio frequencies.
* **Audio Streaming:** It pushes the calculated frequencies directly into a live audio stream for real-time playback.

---

## Where to Find ABC Files

If you need music files to test out the synthesiser, check out these resources:
* **[ABC Notation Official Website](https://abcnotation.com/)**: Visit this site to find thousands of traditional tunes, folk songs, and classical arrangements written entirely in the text-based ABC format.
* **[Beethoven's 16-Voice Symphony No. 7 (Movement 2)](https://www.ucolick.org/~sla/abcmusic/sym7mov2.html)**: Go to this website to download an advanced, highly complex 16-voice/channel arrangement of Beethoven's Symphony No. 7, Movement 2 in an `.abc` file to truly put this synthesiser's polyphony to the test.

---

## Credits & Dependencies

* This project utilizes **[Sokol Audio](https://github.com/floooh/sokol)** for its core, minimal, cross-platform audio stream initialization and playback wrapper. Huge thanks to the creators of the Sokol headers for providing an exceptional utility for low-level audio streaming.