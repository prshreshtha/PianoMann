/*
  ==============================================================================

    PianoMannVoice.h
    Created: 28 Mar 2020 1:52:01am
    Author:  Pranjal Raihan

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <vector>

/**
 * Piano octaves as midi note numbers.
 */
namespace MidiOctaves {
enum {
  kOctave_0 = 21,
  kOctave_1 = kOctave_0 + 12,
  kOctave_2 = kOctave_1 + 12,
  kOctave_3 = kOctave_2 + 12,
  kOctave_4 = kOctave_3 + 12,
  kOctave_5 = kOctave_4 + 12,
  kOctave_6 = kOctave_5 + 12,
  kOctave_7 = kOctave_6 + 12,
};

constexpr int lastNoteFrom(int midiNote) { return midiNote + 3; }
} // namespace MidiOctaves

/**
 * A synth sound backing exactly one note. This is because every key is
 * individually modeled.
 */
struct PianoMannSound : public SynthesiserSound {
  constexpr static int kMinNote = MidiOctaves::kOctave_1;
  constexpr static int kMaxNote =
      MidiOctaves::lastNoteFrom(MidiOctaves::kOctave_6);

  PianoMannSound(int _midiNoteNumber) : midiNoteNumber(_midiNoteNumber) {
    jassert(midiNoteNumber >= kMinNote && midiNoteNumber <= kMaxNote);
  }

  bool appliesToNote(int _midiNoteNumber) override {
    return _midiNoteNumber == this->midiNoteNumber;
  }
  bool appliesToChannel(int midiChannel) override {
    ignoreUnused(midiChannel);
    return true;
  }

private:
  int midiNoteNumber;
};

struct PianoMannVoiceParams {
  /**
   * The midi note number being played. This maps to one single piano key.
   */
  int midiNoteNumber;
};

/**
 * A synth voice that plays one specific note only.
 * This is because each note is modeled differently, albeit with similar
 * techniques.
 */
struct PianoMannVoice : public SynthesiserVoice {
  PianoMannVoice(PianoMannVoiceParams params) : params(params) {}

  bool canPlaySound(SynthesiserSound *sound) override {
    if (auto *pianoMannSound = dynamic_cast<PianoMannSound *>(sound)) {
      return pianoMannSound->appliesToNote(params.midiNoteNumber);
    }
    return false;
  }

  void setCurrentPlaybackSampleRate(double newRate) override {
    SynthesiserVoice::setCurrentPlaybackSampleRate(newRate);
    if (newRate != 0.0) {
      prepareExcitationBuffers();
    }
  }

  void startNote(int midiNoteNumber, float velocity, SynthesiserSound *,
                 int currentPitchWheelPosition) override {
    ignoreUnused(currentPitchWheelPosition);
    jassert(midiNoteNumber == params.midiNoteNumber);
    jassert(isExcitationBufferReady);
    ignoreUnused(midiNoteNumber);
    currentNoteVelocity = velocity;
    isNoteHeld = true;
    tailOff = 0.f;
    exciteBuffer();
  }

  void stopNote(float velocity, bool allowTailOff) override {
    ignoreUnused(velocity);
    if (allowTailOff) {
      if (tailOff == 0.f) {
        tailOff = 1.0f;
      }
    } else {
      clearCurrentNote();
    }
    isNoteHeld = false;
  }

  /**
   * The Karplus-Strong synthesis algorithm we have uses a two-point weighted average
   * filter. The value returned here determines the weight of the `current` sample.
   * The filter is defined as
   * ```
   * let S = return value;
   * y[t] = S*x[t] + (1-S)*x[t-1]
   * ```
   */
  static constexpr float getWeightedAverageFilterForNote(int midiNoteNumber) {
    if (midiNoteNumber <= MidiOctaves::kOctave_0 + 6) {
      return 0.43f;
    }
    if (midiNoteNumber >= MidiOctaves::kOctave_5) {
      return 0.85f;
    }
    return 0.7f;
  }

  struct DecaySpec {
    /**
     * The decay rate for the note's sustain. Value must be [0, 1].
     */
    float sustain;
    /**
     * The additional decay rate after the note is released. This is used to
     * slowly fade out a note upon release. Value must be [0, 1].
     */
    float release;
  };
  /**
   * Gets the sustain and release parameters given a midi note number.
   */
  static constexpr DecaySpec getDecayForNote(int midiNoteNumber) {
    constexpr auto kRelease = 0.992f;
    if (midiNoteNumber >= MidiOctaves::kOctave_5) {
      return DecaySpec{0.9992f, kRelease};
    }
    return DecaySpec{0.997f, kRelease};
  }

  void renderNextBlock(AudioBuffer<float> &outputBuffer, int startSample,
                       int numSamples) override {
    const static auto weightedAverageFilterFactor =
        getWeightedAverageFilterForNote(params.midiNoteNumber);
    const static auto decaySpec = getDecayForNote(params.midiNoteNumber);

    constexpr auto kDecayPowerLevelThreshold = 0.005f;

    if (!isNoteHeld && tailOff == 0.f) {
      // Not playing note nor releasing it slowly
      return;
    }

    if (tailOff > 0.f) {
      // tailOff > 0.f implies we are releasing this note slowly.
      tailOff *= decaySpec.release;
      if (tailOff < kDecayPowerLevelThreshold) {
        tailOff = 0.f;
        clearCurrentNote();
        return;
      }
    }

    for (auto sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
      const auto nextBufferPosition = (1 + currentBufferPosition) %
                                      static_cast<int>(delayLineBuffer.size());

      const auto weightedNextDelaySample =
          weightedAverageFilterFactor * delayLineBuffer[nextBufferPosition];
      const auto weightCurrentDelaySample =
          (1 - weightedAverageFilterFactor) *
          delayLineBuffer[currentBufferPosition];

      auto decay = decaySpec.sustain;
      if (tailOff > 0.f) {
        decay *= tailOff;
      }
      delayLineBuffer[nextBufferPosition] =
          decay * (weightedNextDelaySample + weightCurrentDelaySample);

      const auto currentSample = delayLineBuffer[currentBufferPosition];
      for (auto channel = outputBuffer.getNumChannels(); --channel >= 0;) {
        auto *output = outputBuffer.getWritePointer(channel, startSample);
        output[sampleIndex] += currentSample;
      }

      currentBufferPosition = nextBufferPosition;
    }
  }

  using SynthesiserVoice::renderNextBlock;

  void pitchWheelMoved(int newValue) override { ignoreUnused(newValue); }
  void controllerMoved(int controllerNumber, int newValue) override {
    ignoreUnused(controllerNumber, newValue);
  }

private:
  /**
   * Set up the delay-line as shown in Karplus-Strong. The length of the delay line determines
   * the frequency of note played.
   */
  void prepareExcitationBuffers() {
    const auto sampleRate = getSampleRate();
    jassert(sampleRate != 0.0);

    const auto frequencyInHz =
        MidiMessage::getMidiNoteInHertz(params.midiNoteNumber);
    const auto excitationNumSamples = roundToInt(sampleRate / frequencyInHz);

    delayLineBuffer.resize(excitationNumSamples);
    std::fill(delayLineBuffer.begin(), delayLineBuffer.end(), 0.f);

    excitationBuffer.resize(excitationNumSamples);
    std::generate(excitationBuffer.begin(), excitationBuffer.end(), [] {
      return (Random::getSystemRandom().nextFloat() * 2.0f) - 1.0f;
    });

    currentBufferPosition = 0;
    isExcitationBufferReady = true;
  }

  /**
   * Create burst of "noise" seeding the Karplus-Strong feedback loop. Since this feeds the delay line,
   * it must be equal or smaller in length (preferably the same size).
   */
  void exciteBuffer() {
    jassert(delayLineBuffer.size() >= excitationBuffer.size());

    std::transform(excitationBuffer.begin(), excitationBuffer.end(),
                   delayLineBuffer.begin(),
                   [this](float sample) { return currentNoteVelocity * sample; });
  }

  /**
   * The string synthesis constant parameters.
   */
  const PianoMannVoiceParams params;
  /**
   * The velocity of the currently played note.
   */
  float currentNoteVelocity = 0.f;

  bool isExcitationBufferReady = false;
  std::vector<float> excitationBuffer, delayLineBuffer;
  /**
   * The delay line buffer is a feedback loop and so the array behaves as a ring buffer. This tracks
   * the current position in the ring buffer.
   */
  int currentBufferPosition = 0;

  /**
   * Whether or not the currently playing note is held down right now. Upon release, this is `false`
   * but there might still be some sound created after release.
   */
  bool isNoteHeld = false;
  /**
   * The current value of the decay that starts after a note's release. A value of (approximately) 0 means that the decay
   * is (mostly) complete. A value of 1 is used to initialize the tail off.
   */
  float tailOff = 0.f;
};
