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
		return scanLengthText(getValue(), m ? m->activeBank.load() : nullptr);
	}
	std::string getUnit() override {
		return "";
	}
};
