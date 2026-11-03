#ifndef S30_MIXER_H
#define S30_MIXER_H

// The N64 audio microcode (aspMain) in software (n64/src/mixer.c): DMEM, the
// ADPCM decoder, resampler, envelope mixer and the rest. Each game redirects
// its Acmd macros (aClearBuffer and so on, whose arguments differ between
// games) to these functions in a header of its own, so its synthesis code runs
// them directly instead of building a command list.
//
// Plain C types: game code includes this, and some games' headers clash with
// <stdint.h>.

// A decoder or resampler state: 16 samples
typedef short S30MixState[16];

void aClearBufferImpl(unsigned short addr, int nbytes);
// copy: 1 copies into DMEM; 0 leaves the data in place for the decoders (they
// read it from source_addr); 2 copies and points the decoders at DMEM
void aLoadBufferImpl(const void* source_addr, unsigned short dest_addr, unsigned short nbytes, unsigned char copy);
void aSaveBufferImpl(unsigned short source_addr, short* dest_addr, unsigned short nbytes);
void aLoadADPCMImpl(int num_entries_times_16, const short* book_source_addr);
void aSetBufferImpl(unsigned char flags, unsigned short in, unsigned short out, unsigned short nbytes);
void aDMEMMoveImpl(unsigned short in_addr, unsigned short out_addr, int nbytes);
void aSetLoopImpl(S30MixState* adpcm_loop_state);
void aADPCMdecImpl(unsigned char flags, short* state);
void aResampleImpl(unsigned char flags, unsigned short pitch, short* state);
void aEnvSetup1Impl(unsigned char initialVolReverb, unsigned short rampReverb, unsigned short rampLeft,
                    unsigned short rampRight);
void aEnvSetup2Impl(unsigned short initialVolLeft, unsigned short initialVolRight);
void aEnvMixerImpl(unsigned short dmemIn, unsigned short count, unsigned char swapReverb, unsigned char x0,
                   unsigned char x1, unsigned char x2, unsigned char x3, unsigned int dmemDests, unsigned int opBits);
// Interleaves L and R (count bytes each) into out / into the last aSetBuffer's
// output, its byte count per channel
void aInterleaveImpl(unsigned short outAddr, unsigned short inL, unsigned short inR, unsigned short count);
void aInterleaveSetBufferImpl(unsigned short inL, unsigned short inR);
void aInterlImpl(unsigned short in_addr, unsigned short out_addr, unsigned short n_samples);
void aMixImpl(unsigned short count, short gain, unsigned short in_addr, unsigned short out_addr);
void aFilterImpl(unsigned char flags, unsigned short countOrBuf, short* state_or_filter);
void aHiLoGainImpl(unsigned char g, unsigned short count, unsigned short dmemIn);
void aDuplicateImpl(unsigned short count, unsigned short in_addr, unsigned short out_addr);
void aAddMixerImpl(unsigned short count, unsigned short in_addr, unsigned short out_addr, unsigned short a4);
void aS8DecImpl(unsigned char flags, short* state);
void aResampleZohImpl(unsigned short pitch, unsigned short startFract);
void aUnkCmd3Impl(unsigned short a1, unsigned short a2, unsigned short a3);
void aUnkCmd19Impl(unsigned char f, unsigned short count, unsigned short out_addr, unsigned short in_addr);

// <prefix>_AUDIODBG counters: ADPCM decodes, envelope mixes, their peaks
extern int gS30MixDbg[4];

#endif
