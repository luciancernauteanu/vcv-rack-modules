#pragma once
#include "../WaveArtifact.hpp"

/** SCAN LENGTH as text: seconds of the loaded sample, or "to the next marker" at the top of the knob. */
inline std::string scanLengthText(float k, const wa::WavetableBank* bank) {
	if (k >= wa::SCAN_LENGTH_TOP)
		return "to the next marker";
	if (!bank || !bank->sample.valid())
		return string::f("%.0f%% of the way to the whole sample", k / wa::SCAN_LENGTH_TOP * 100.f);
	double seconds = (double) bank->sample.frames / bank->sample.sampleRate;
	double s = wa::grainScanLengthFraction(k, seconds) * seconds;
	return s < 1.0 ? string::f("%.0f ms", s * 1000.0) : string::f("%.2f s", s);
}

/** SCAN LENGTH knob display (see scanLengthText) */
struct GrainScanLengthQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		// SAMPLE with BEAT: a note value at the tempo (straight, plus the marker's T / D)
		int marker = paramId - WaveArtifact::MARKER_GRAIN_SCAN_LENGTH_PARAM;
		if (m && (int) std::round(m->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()) == 2
			&& (int) std::round(m->params[WaveArtifact::MARKER_SAMPLE_BEAT_PARAM + marker].getValue()) == wa::SAMPLE_BEAT_CUT) {
			int d = wa::sampleBeatDivision(getValue(), m->params[WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + marker].getValue() > 0.5f,
				m->params[WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + marker].getValue() > 0.5f);
			return string::f("%s (%g beats)", wa::SAMPLE_DIVISIONS[d].name, std::round(wa::SAMPLE_DIVISIONS[d].beats * 1000.f) / 1000.f);
		}
		return scanLengthText(getValue(), m ? m->bankOf(marker) : nullptr);
	}
	std::string getUnit() override {
		return "";
	}
};
