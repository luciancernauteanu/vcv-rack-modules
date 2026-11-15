#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	p->addModel(modelWaveArtifact);
	p->addModel(modelWaveArtifactExpander);
	p->addModel(modelWaveArtifactSequencer);
	p->addModel(modelWaveArtifactSendReturn);
}
