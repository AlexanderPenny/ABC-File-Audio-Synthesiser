#define SOKOL_IMPL
#include "sokol_audio.h"

#define _USE_MATH_DEFINES
#include <cmath>

#include <iostream>
#include <string>
#include <vector>
#include <atomic>

#include "ABC_Parser.h"
#include "ABC_Note.h"

//------------------------------------------------------------------------------
// noteToFreq
//	Converts an ABCNote into a frequency in Hz using equal temperament with
//	A4 = 440 Hz as the reference. This mirrors the private letterToFreq logic
//	from ABCParser but lives here so the audio callback can call it freely.
//------------------------------------------------------------------------------
static float noteToFreq(const ABCNote& note)
{
	// Semitone offsets from C, indexed by letter A=0 .. G=6
	static const int semitones[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G

	int semi = semitones[note.letter - 'A'];

	switch (note.accidental)
	{
	case Accidental::Sharp:       semi += 1; break;
	case Accidental::Flat:        semi -= 1; break;
	case Accidental::DoubleSharp: semi += 2; break;
	case Accidental::DoubleFlat:  semi -= 2; break;
	default: break;
	}

	// Distance in semitones from A4 (octave 1, letter A in the ABC scheme)
	int semiFromA4 = semi - 9 + ((note.octave - 1) * 12);
	return (float)(440.0 * std::pow(2.0, semiFromA4 / 12.0));
}

//------------------------------------------------------------------------------
// ADSREnvelope
//	Shapes the amplitude of a voice over time through four stages:
//	Attack (fade in), Decay (drop to sustain level), Sustain (held level),
//	Release (fade out after the note ends). Multiply your oscillator output
//	by the value returned from process() each sample.
//------------------------------------------------------------------------------
enum class EnvStage { Idle, Attack, Decay, Sustain, Release };

struct ADSREnvelope
{
	EnvStage stage = EnvStage::Idle;
	float    value = 0.0f;  // current amplitude, 0..1

	// These are set globally on AudioData and copied in when a voice triggers,
	// so all voices share the same ADSR shape (which can be tweaked at runtime)
	float attack = 0.005f; // seconds to reach full amplitude
	float decay = 0.4f; // seconds to fall from peak to sustain level
	float sustain = 0.15f;// amplitude level held while the note is sounding
	float release = 5.0f; // seconds to fade out after the note ends

	void noteOn() { stage = EnvStage::Attack; }
	void noteOff() { if (stage != EnvStage::Idle) stage = EnvStage::Release; }

	// Call once per sample; returns the current amplitude multiplier
	float process(float sampleRate)
	{
		switch (stage)
		{
		case EnvStage::Attack:
			value += 1.0f / (attack * sampleRate);
			if (value >= 1.0f) { value = 1.0f; stage = EnvStage::Decay; }
			break;

		case EnvStage::Decay:
			value -= (1.0f - sustain) / (decay * sampleRate);
			if (value <= sustain) { value = sustain; stage = EnvStage::Sustain; }
			break;

		case EnvStage::Sustain:
			value = sustain; // hold here until noteOff() kicks us into Release
			break;

		case EnvStage::Release:
			value -= value / (release * sampleRate);  // decay from wherever
			if (value <= 0.001f) { value = 0.0f; stage = EnvStage::Idle; }
			break;

		default:
			break;
		}
		return value;
	}

	bool isIdle() const { return stage == EnvStage::Idle; }
};

//------------------------------------------------------------------------------
// Voice
//	One independent sine oscillator with its own ADSR envelope and note-timing
//	state. AudioData holds a fixed pool of MAX_VOICES of these; the scheduler
//	grabs a free one each time a new note needs to start.
//------------------------------------------------------------------------------
static constexpr int MAX_VOICES = 128;
static constexpr float MIX_NORMALISER = 16.0f;

struct Voice
{
	bool  active = false;  // true while this voice is playing or releasing
	float freq = 0.0f;  // frequency in Hz for this note
	float sinIndex = 0.0f;  // current oscillator phase in radians

	// Note timing -- measured in samples against the shared sample rate
	int   samplesElapsed = 0;
	int   noteLenSamples = 0;   // how long the pitched tone lasts
	int   gapLenSamples = 0;   // silence after the tone (before the next note on this voice)
	bool  inGap = false;

	ADSREnvelope env;
};

//------------------------------------------------------------------------------
// AudioData
//	All state shared between main() and the audio callback. Fields that are
//	written from main() and read in the callback are std::atomic to avoid
//	data races without needing a mutex.
//------------------------------------------------------------------------------
struct AudioData
{
	// --- ABC score -----------------------------------------------------------
	const std::vector<ABCNote>* notes = nullptr;  // read-only pointer to parsed note list
	double bpm = 120.0;

	// --- Sequencer state -----------------------------------------------------
	// The sequencer runs in the audio callback. It steps through allNotes in
	// order and fires a new Voice for each one as its start time arrives.
	int    sequencerNote = 0;    // index of the next note to schedule
	int    sequencerSample = 0;    // sample counter for the sequencer cursor
	int    nextNoteAtSample = 0;    // the sample number at which to fire the next note

	// --- Voice pool ----------------------------------------------------------
	Voice  voices[MAX_VOICES];

	// --- Global ADSR parameters (copied into each voice when it triggers) ----
	// These can be changed from main() between notes safely since the callback
	// only reads them at the moment a new voice is spawned.
	float  adsrAttack = 0.005f;
	float  adsrDecay = 0.4f;
	float  adsrSustain = 0.15f;
	float  adsrRelease = 1.0f;

	// --- Mix -----------------------------------------------------------------
	float  masterLevel = 0.15f;  // overall output amplitude before summing voices

	// --- Control -------------------------------------------------------------
	std::atomic<bool> playing{ false };
};

//------------------------------------------------------------------------------
static int beatsToSamples(double beats, double bpm, float sampleRate)
{
	double secondsPerBeat = 60.0 / bpm;
	return (int)(beats * secondsPerBeat * sampleRate);
}

//------------------------------------------------------------------------------
// findFreeVoice
//	Returns a pointer to the first idle Voice in the pool, or nullptr if all
//	MAX_VOICES are currently active. I keep this pool bigger than the abc voice
//	count so notes can fade out without blocking the next notes.
//------------------------------------------------------------------------------
static Voice* findFreeVoice(AudioData* data)
{
	for (int i = 0; i < MAX_VOICES; ++i)
		if (!data->voices[i].active)
			return &data->voices[i];
	return nullptr;
}

//------------------------------------------------------------------------------
// triggerVoice
//	Initialises a free voice for a given ABCNote, copying the current global
//	ADSR parameters in so all active voices share the same envelope shape.
//------------------------------------------------------------------------------
static void triggerVoice(AudioData* data, const ABCNote& note, float sampleRate)
{
	if (note.isRest) return;  // Rests occupy time but produce no voice

	Voice* v = findFreeVoice(data);
	if (!v) return;  // All voices busy; this note will be silently dropped

	v->active = true;
	v->freq = noteToFreq(note);
	v->sinIndex = 0.0f;
	v->samplesElapsed = 0;
	v->noteLenSamples = beatsToSamples(note.durationBeats, data->bpm, sampleRate);

	v->gapLenSamples = (int)(data->adsrRelease * sampleRate);

	v->inGap = false;

	// Copy current global ADSR settings into this voice
	v->env.attack = data->adsrAttack;
	v->env.decay = data->adsrDecay;
	v->env.sustain = data->adsrSustain;
	v->env.release = data->adsrRelease;
	v->env.stage = EnvStage::Idle;
	v->env.value = 0.0f;
	v->env.noteOn();
}

//------------------------------------------------------------------------------
// audioCallback
//	Called by sokol_audio on a background thread to fill each audio buffer.
//	The sequencer at the top schedules new voices in sample-accurate time;
//	the voice loop below synthesises and mixes all active voices.
//------------------------------------------------------------------------------
void audioCallback(float* buffer, int numFrames, int numChannels, void* userData)
{
	AudioData* data = (AudioData*)userData;
	if (!data) return;

	const float sampleRate = (float)saudio_sample_rate();

	for (int i = 0; i < numFrames; ++i)
	{
		buffer[i] = 0.0f;

		if (!data->playing || !data->notes) continue;

		// --- Sequencer: fire voices at the correct sample time ---------------
		// Keep launching notes that are due at or before the current sample.
		// Chord notes (chordStart == false) are fired immediately alongside the
		// previous note without advancing the clock -- they share the same start
		// sample. Only a chordStart == true note moves nextNoteAtSample forward.
		while (data->sequencerNote < (int)data->notes->size()
			&& data->sequencerSample >= data->nextNoteAtSample)
		{
			const ABCNote& note = (*data->notes)[data->sequencerNote];

			triggerVoice(data, note, sampleRate);

			// Only advance the sequencer clock for notes that start a new time
			// position. Chord continuation notes share the clock position of the
			// first note in the chord, so we skip the advance for them.
			if (note.chordStart)
			{
				data->nextNoteAtSample += beatsToSamples(
					note.durationBeats + note.gapBeats, data->bpm, sampleRate);
			}

			data->sequencerNote++;
		}

		// Stop playback once every note has been scheduled and all voices finish
		if (data->sequencerNote >= (int)data->notes->size())
		{
			// Check whether any voice is still sounding
			bool anyActive = false;
			for (int v = 0; v < MAX_VOICES; ++v)
				if (data->voices[v].active) { anyActive = true; break; }

			if (!anyActive)
			{
				data->playing = false;
				continue;
			}
		}

		data->sequencerSample++;

		// --- Voice synthesis loop: mix all active voices into this sample ----
		float mix = 0.0f;

		for (int v = 0; v < MAX_VOICES; ++v)
		{
			Voice& voice = data->voices[v];
			if (!voice.active) continue;

			voice.samplesElapsed++;

			// Transition from tone to gap when the note duration has elapsed;
			// this triggers the envelope release so the note fades out naturally
			if (!voice.inGap && voice.samplesElapsed >= voice.noteLenSamples)
			{
				voice.inGap = true;
				voice.samplesElapsed = 0;
				voice.env.noteOff();  // Start the release stage of the envelope
			}

			// Once the gap has also elapsed, this voice slot is free again
			if (voice.inGap && voice.samplesElapsed >= voice.gapLenSamples
				&& voice.env.isIdle())
			{
				voice.active = false;
				continue;
			}

			// Synthesise: sine wave * envelope amplitude
			float envAmp = voice.env.process(sampleRate);

			float fundamental = sinf(voice.sinIndex);
			float second = sinf(voice.sinIndex * 2.0f) * 0.5f;
			float third = sinf(voice.sinIndex * 3.0f) * 0.25f;
			mix += (fundamental + second + third) * envAmp;

			// Advance and wrap the oscillator phase
			voice.sinIndex += (voice.freq / sampleRate) * 2.0f * (float)M_PI;
			voice.sinIndex = fmodf(voice.sinIndex, 2.0f * (float)M_PI);
		}

	// I divide by the intended 16 abc voices, not the larger playback pool.
	buffer[i] = (mix / MIX_NORMALISER) * data->masterLevel;
	}
}

//------------------------------------------------------------------------------
int main(int argc, char** argv)
{
	ABCParser parser;

	if (!parser.loadFromFile("abc/aether_foundry_16_voice_dense.abc")) {
		std::cerr << "FATAL ERROR: " << parser.getLastError() << std::endl;
		system("pause");
		return -1;
	}

	const ABCScore& score = parser.getScore();
	std::cout << "Loaded: " << score.title
		<< " (" << score.allNotes.size() << " notes, "
		<< score.bpm << " bpm)" << std::endl;

	// ---------- DEBUGGING segment ---------- //
	/*std::cout << "\n--- First 32 notes (idx | chordStart | dur | letter) ---\n";
	int debugLimit = std::min((int)score.allNotes.size(), 32);
	for (int di = 0; di < debugLimit; ++di)
	{
		const ABCNote& n = score.allNotes[di];
		std::cout << "  [" << di << "] chordStart=" << n.chordStart
			<< "  dur=" << n.durationBeats
			<< "  " << (n.isRest ? "REST" : std::string(1, n.letter))
			<< " oct=" << n.octave << "\n";
	}
	std::cout << "---\n\n";*/
	// --------------------------------------- //

	AudioData audioData;
	audioData.notes = &score.allNotes;
	audioData.bpm = score.bpm;

	saudio_desc audioDescriptor = {};
	audioDescriptor.num_channels = 1;
	audioDescriptor.stream_userdata_cb = audioCallback;
	audioDescriptor.user_data = (void*)&audioData;

	saudio_setup(&audioDescriptor);

	if (!saudio_isvalid())
	{
		std::cout << "Could not initialise sokol_audio. Exiting." << std::endl;
		return 1;
	}

	audioData.playing = true;

	std::cout << "Playing... (press x <enter> to quit)" << std::endl;

	char input = 0;
	while ((input = (char)std::cin.get()) != 'x') {}

	saudio_shutdown();
	return 0;
}
