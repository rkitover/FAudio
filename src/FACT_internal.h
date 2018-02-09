/* FACT - XACT Reimplementation for FNA
 * Copyright 2009-2018 Ethan Lee and the MonoGame Team
 *
 * Released under the Microsoft Public License.
 * See LICENSE for details.
 */

#include "FACT.h"
#include "FACT3D.h"
#include "FAudio_internal.h"

/* TODO: Remove this entirely */
typedef uint32_t fixed32;
typedef struct FAudioResampleState
{
	/* Checked against wave->pitch for redundancy */
	uint16_t pitch;

	fixed32 step;
	fixed32 offset;

	/* Padding used for smooth resampling from block to block */
	int16_t padding[2][1];
} FAudioResampleState;
void FAudio_INTERNAL_InitResampler(FAudioResampleState *resample);

/* Internal Constants */

#define FACT_VOLUME_0 180

/* Internal AudioEngine Types */

typedef struct FACTAudioEngineCallback
{
	FAudioEngineCallback callback;
	FACTAudioEngine *engine;
} FACTAudioEngineCallback;

typedef struct FACTAudioCategory
{
	uint8_t instanceLimit;
	uint16_t fadeInMS;
	uint16_t fadeOutMS;
	uint8_t maxInstanceBehavior;
	int16_t parentCategory;
	float volume;
	uint8_t visibility;
	uint8_t instanceCount;
	float currentVolume;
} FACTAudioCategory;

typedef struct FACTVariable
{
	uint8_t accessibility;
	float initialValue;
	float minValue;
	float maxValue;
} FACTVariable;

typedef struct FACTRPCPoint
{
	float x;
	float y;
	uint8_t type;
} FACTRPCPoint;

typedef enum FACTRPCParameter
{
	RPC_PARAMETER_VOLUME,
	RPC_PARAMETER_PITCH,
	RPC_PARAMETER_REVERBSEND,
	RPC_PARAMETER_FILTERFREQUENCY,
	RPC_PARAMETER_FILTERQFACTOR,
	RPC_PARAMETER_COUNT /* If >=, DSP Parameter! */
} FACTRPCParameter;

typedef struct FACTRPC
{
	uint16_t variable;
	uint8_t pointCount;
	uint16_t parameter;
	FACTRPCPoint *points;
} FACTRPC;

typedef struct FACTDSPParameter
{
	uint8_t type;
	float value;
	float minVal;
	float maxVal;
	uint16_t unknown;
} FACTDSPParameter;

typedef struct FACTDSPPreset
{
	uint8_t accessibility;
	uint32_t parameterCount;
	FACTDSPParameter *parameters;
} FACTDSPPreset;

/* Internal SoundBank Types */

typedef struct FACTCueData
{
	uint8_t flags;
	uint32_t sbCode;
	uint32_t transitionOffset;
	uint8_t instanceLimit;
	uint16_t fadeInMS;
	uint16_t fadeOutMS;
	uint8_t maxInstanceBehavior;
	uint8_t instanceCount;
} FACTCueData;

typedef enum
{
	FACTEVENT_STOP =				0,
	FACTEVENT_PLAYWAVE =				1,
	FACTEVENT_PLAYWAVETRACKVARIATION =		3,
	FACTEVENT_PLAYWAVEEFFECTVARIATION =		4,
	FACTEVENT_PLAYWAVETRACKEFFECTVARIATION =	6,
	FACTEVENT_PITCH =				7,
	FACTEVENT_VOLUME =				8,
	FACTEVENT_MARKER =				9,
	FACTEVENT_PITCHREPEATING =			16,
	FACTEVENT_VOLUMEREPEATING =			17,
	FACTEVENT_MARKERREPEATING =			18
} FACTEventType;

typedef struct FACTSimpleWave
{
	uint16_t track;
	uint8_t wavebank;
} FACTSimpleWave;

typedef struct FACTEvent_PlayWave
{
	uint8_t flags;
	uint16_t position;
	uint16_t angle;

	/* Track Variation */
	uint8_t isComplex;
	union
	{
		FACTSimpleWave simple;
		struct
		{
			uint16_t variation;
			uint16_t trackCount;
			uint16_t *tracks;
			uint8_t *wavebanks;
			uint8_t *weights;
		} complex;
	};

	/* Effect Variation */
	int16_t minPitch;
	int16_t maxPitch;
	float minVolume;
	float maxVolume;
	float minFrequency;
	float maxFrequency;
	float minQFactor;
	float maxQFactor;
	uint16_t variationFlags;
} FACTEvent_PlayWave;

typedef struct FACTEvent_SetValue
{
	uint8_t settings;
	union
	{
		struct
		{
			float initialValue;
			float initialSlope;
			float slopeDelta;
			uint16_t duration;
		} ramp;
		struct
		{
			uint8_t flags;
			float value1;
			float value2;
		} equation;
	};
} FACTEvent_SetValue;

typedef struct FACTEvent_Stop
{
	uint8_t flags;
} FACTEvent_Stop;

typedef struct FACTEvent_Marker
{
	uint32_t marker;
	uint8_t repeating;
} FACTEvent_Marker;

typedef struct FACTEvent
{
	uint16_t type;
	uint16_t timestamp;
	uint16_t randomOffset;
	uint8_t loopCount;
	uint16_t frequency;
	union
	{
		FACTEvent_PlayWave wave;
		FACTEvent_SetValue value;
		FACTEvent_Stop stop;
		FACTEvent_Marker marker;
	};
} FACTEvent;

typedef struct FACTTrack
{
	uint32_t code;

	float volume;
	uint8_t filter;
	uint8_t qfactor;
	uint16_t frequency;

	uint8_t rpcCodeCount;
	uint32_t *rpcCodes;

	uint8_t eventCount;
	FACTEvent *events;
} FACTTrack;

typedef struct FACTSound
{
	uint8_t flags;
	uint16_t category;
	float volume;
	int16_t pitch;
	uint8_t priority;

	uint8_t trackCount;
	uint8_t rpcCodeCount;
	uint8_t dspCodeCount;

	FACTTrack *tracks;
	uint32_t *rpcCodes;
	uint32_t *dspCodes;
} FACTSound;

typedef struct FACTInstanceRPCData
{
	float rpcVolume;
	float rpcPitch;
	float rpcFilterFreq;
} FACTInstanceRPCData;

typedef struct FACTEventInstance
{
	uint16_t timestamp;
	uint8_t loopCount;
	uint8_t finished;
	union
	{
		struct
		{
			FACTWave *wave;
			float baseVolume;
			int16_t basePitch;
			float baseQFactor;
			float baseFrequency;
		} wave;
		float value;
	} data;
} FACTEventInstance;

typedef struct FACTTrackInstance
{
	/* Tracks which events have fired */
	FACTEventInstance *events;

	/* RPC instance data */
	FACTInstanceRPCData rpcData;
} FACTTrackInstance;

typedef struct FACTSoundInstance
{
	/* Base Sound reference */
	FACTSound *sound;

	/* Per-instance track information */
	FACTTrackInstance *tracks;

	/* RPC instance data */
	FACTInstanceRPCData rpcData;
} FACTSoundInstance;

typedef struct FACTVariation
{
	union
	{
		FACTSimpleWave simple;
		uint32_t soundCode;
	};
	float minWeight;
	float maxWeight;
	uint32_t linger;
} FACTVariation;

typedef struct FACTVariationTable
{
	uint8_t flags;
	int16_t variable;
	uint8_t isComplex;

	uint16_t entryCount;
	FACTVariation *entries;
} FACTVariationTable;

/* Internal Wave Types */

typedef uint32_t (FACTCALL * FACTDecodeCallback)(
	FACTWave *wave,
	int16_t *decodeCacheL,
	int16_t *decodeCacheR,
	uint32_t samples
);

/* Public XACT Types */

struct FACTAudioEngine
{
	uint16_t categoryCount;
	uint16_t variableCount;
	uint16_t rpcCount;
	uint16_t dspPresetCount;
	uint16_t dspParameterCount;

	char **categoryNames;
	char **variableNames;
	uint32_t *rpcCodes;
	uint32_t *dspPresetCodes;

	FACTAudioCategory *categories;
	FACTVariable *variables;
	FACTRPC *rpcs;
	FACTDSPPreset *dspPresets;

	/* Engine references */
	FACTSoundBank *sbList;
	FACTWaveBank *wbList;
	float *globalVariableValues;

#if 0 /* TODO: FAudio */
	/* FAudio references */
	FAudio *audio;
	FAudioMasteringVoice *master;
	FACTAudioEngineCallback callback;
#else
	/* Point this to your platform's device mix format */
	FAudioWaveFormatExtensible *mixFormat;
#endif
};

struct FACTSoundBank
{
	/* Engine references */
	FACTAudioEngine *parentEngine;
	FACTSoundBank *next;
	FACTCue *cueList;

	/* Array sizes */
	uint16_t cueCount;
	uint8_t wavebankCount;
	uint16_t soundCount;
	uint16_t variationCount;

	/* Strings, strings everywhere! */
	char **wavebankNames;
	char **cueNames;

	/* Actual SoundBank information */
	char *name;
	FACTCueData *cues;
	FACTSound *sounds;
	uint32_t *soundCodes;
	FACTVariationTable *variations;
	uint32_t *variationCodes;
};

struct FACTWaveBank
{
	/* Engine references */
	FACTAudioEngine *parentEngine;
	FACTWave *waveList;
	FACTWaveBank *next;

	/* Actual WaveBank information */
	char *name;
	uint32_t entryCount;
	FACTWaveBankEntry *entries;
	uint32_t *entryRefs;

	/* I/O information */
	uint16_t streaming;
	FACTIOStream *io;
};

struct FACTWave
{
	/* Engine references */
	FACTWaveBank *parentBank;
	FACTWave *next;
	uint16_t index;

	/* Playback */
	uint32_t state;
	float volume;
	int16_t pitch;
	uint32_t position;
	uint32_t initialPosition;
	uint8_t loopCount;

	/* 3D Data */
	uint32_t srcChannels;
	uint32_t dstChannels;
	float matrixCoefficients[2 * 8]; /* Stereo input, 7.1 output */

	/* Decoding */
	FACTDecodeCallback decode;
	FAudioResampleState resample;
	int16_t msadpcmCache[1024];
	uint16_t msadpcmExtra;
};

struct FACTCue
{
	/* Engine references */
	FACTSoundBank *parentBank;
	FACTCue *next;
	uint8_t managed;
	uint16_t index;

	/* Sound data */
	FACTCueData *data;
	union
	{
		FACTSound *sound;
		FACTVariationTable *variation;
	} sound;

	/* Instance data */
	float *variableValues;

	/* Playback */
	uint32_t state;
	uint8_t active; /* 0x01 for wave, 0x02 for sound */
	union
	{
		FACTWave *wave;
		FACTSoundInstance sound;
	} playing;
	FACTVariation *playingVariation;

	/* 3D Data */
	uint8_t active3D;
	uint32_t srcChannels;
	uint32_t dstChannels;
	float matrixCoefficients[2 * 8]; /* Stereo input, 7.1 output */

	/* Timer */
	uint32_t start;
	uint32_t elapsed;
};

/* Internal functions */

void FACT_INTERNAL_OnProcessingPassStart(FAudioEngineCallback *callback);

/* TODO: Remove these entirely */
void FACT_PlatformInitEngine(FACTAudioEngine *engine, int16_t *id);
void FACT_PlatformCloseEngine(FACTAudioEngine *engine);

/* TODO: Remove these from the header */
void FACT_INTERNAL_UpdateEngine(FACTAudioEngine *engine);
void FACT_INTERNAL_UpdateCue(FACTCue *cue, uint32_t elapsed);

/* TODO: Move this to FAudio */
uint32_t FACT_INTERNAL_GetWave(
	FACTWave *wave,
	int16_t *decodeCacheL,
	int16_t *decodeCacheR,
	float *resampleCacheL,
	float *resampleCacheR,
	uint32_t samples
);

float FACT_INTERNAL_CalculateAmplitudeRatio(float decibel);
void FACT_INTERNAL_SelectSound(FACTCue *cue);
void FACT_INTERNAL_BeginFadeIn(FACTCue *cue);
void FACT_INTERNAL_BeginFadeOut(FACTCue *cue);

/* TODO: Move this to FAudio */
#define DECODE_FUNC(type) \
	extern uint32_t FACT_INTERNAL_Decode##type( \
		FACTWave *wave, \
		int16_t *decodeCacheL, \
		int16_t *decodeCacheR, \
		uint32_t samples \
	);
DECODE_FUNC(MonoPCM8)
DECODE_FUNC(MonoPCM16)
DECODE_FUNC(MonoMSADPCM)
DECODE_FUNC(StereoPCM8)
DECODE_FUNC(StereoPCM16)
DECODE_FUNC(StereoMSADPCM)
#undef DECODE_FUNC

typedef size_t (FACTCALL * FACT_readfunc)(
	void *data,
	void *dst,
	size_t size,
	size_t count
);
typedef int64_t (FACTCALL * FACT_seekfunc)(
	void *data,
	int64_t offset,
	int whence
);
typedef int (FACTCALL * FACT_closefunc)(
	void *data
);

struct FACTIOStream
{
	void *data;
	FACT_readfunc read;
	FACT_seekfunc seek;
	FACT_closefunc close;
};
