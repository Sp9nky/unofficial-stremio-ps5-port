// Link stub for libSceVideodec2 (the hardware video decoder), which the
// payload SDK has no stub for. native/build.sh builds it into
// libSceVideodec2.so (SONAME libSceVideodec2.sprx); the converter turns each
// call into an import of the console's module, loaded at start-up by
// src/hwdec_ps5.cpp. Only the names matter; the bodies never run.
// Names: SvenGDK/SharpProspero, as listed by Nuvio PS5.
void sceVideodec2QueryComputeMemoryInfo(void) {}
void sceVideodec2AllocateComputeQueue(void) {}
void sceVideodec2ReleaseComputeQueue(void) {}
void sceVideodec2QueryDecoderMemoryInfo(void) {}
void sceVideodec2CreateDecoder(void) {}
void sceVideodec2DeleteDecoder(void) {}
void sceVideodec2MapDirectMemory(void) {}
void sceVideodec2Reset(void) {}
void sceVideodec2Decode(void) {}
void sceVideodec2Flush(void) {}
