//  Copyright (c) 2007-2009 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include <string.h>
#include <errno.h>

#include "utils.h"
#include "indexing.h"

#ifdef _MSC_VER
#	include <intrin.h>
#endif

// Export the array but not its data type... fun...
typedef struct CodecTags{
    char str[20];
    enum CodecID id;
} CodecTags;

extern "C" {
extern const AVCodecTag codec_bmp_tags[];
extern const CodecTags ff_mkv_codec_tags[];
extern const AVCodecTag codec_movvideo_tags[];
extern const AVCodecTag codec_wav_tags[];
}

int GetCPUFlags() {
// FIXME Add proper feature detection when msvc isn't used
	int Flags = PP_CPU_CAPS_MMX | PP_CPU_CAPS_MMX2;

#ifdef _MSC_VER
	Flags = 0;
	int CPUInfo[4];
	__cpuid(CPUInfo, 0);

	// PP and SWS defines have the same values for their defines so this actually works
	if (CPUInfo[3] & (1 << 23))
		Flags |= PP_CPU_CAPS_MMX;
	if (CPUInfo[3] & (1 << 25))
		Flags |= PP_CPU_CAPS_MMX2;
#endif

	return Flags;
}

FFMS_TrackType HaaliTrackTypeToFFTrackType(int TT) {
	switch (TT) {
		case TT_VIDEO: return FFMS_TYPE_VIDEO; break;
		case TT_AUDIO: return FFMS_TYPE_AUDIO; break;
		case TT_SUB: return FFMS_TYPE_SUBTITLE; break;
		default: return FFMS_TYPE_UNKNOWN;
	}
}

int ReadFrame(uint64_t FilePos, unsigned int &FrameSize, CompressedStream *CS, MatroskaReaderContext &Context, char *ErrorMsg, unsigned MsgSize) {
	if (CS) {
		char CSBuffer[4096];

		unsigned int DecompressedFrameSize = 0;

		cs_NextFrame(CS, FilePos, FrameSize);

		for (;;) {
			int ReadBytes = cs_ReadData(CS, CSBuffer, sizeof(CSBuffer));
			if (ReadBytes < 0) {
				snprintf(ErrorMsg, MsgSize, "Error decompressing data: %s", cs_GetLastError(CS));
				return 1;
			}
			if (ReadBytes == 0) {
				FrameSize = DecompressedFrameSize;
				return 0;
			}

			if (Context.BufferSize < DecompressedFrameSize + ReadBytes) {
				Context.BufferSize = FrameSize;
				Context.Buffer = (uint8_t *)realloc(Context.Buffer, Context.BufferSize + 16);
				if (Context.Buffer == NULL)  {
					snprintf(ErrorMsg, MsgSize, "Out of memory");
					return 2;
				}
			}

			memcpy(Context.Buffer + DecompressedFrameSize, CSBuffer, ReadBytes);
			DecompressedFrameSize += ReadBytes;
		}
	} else {
		if (fseeko(Context.ST.fp, FilePos, SEEK_SET)) {
			snprintf(ErrorMsg, MsgSize, "fseek(): %s", strerror(errno));
			return 3;
		}

		if (Context.BufferSize < FrameSize) {
			Context.BufferSize = FrameSize;
			Context.Buffer = (uint8_t *)realloc(Context.Buffer, Context.BufferSize + 16);
			if (Context.Buffer == NULL) {
				snprintf(ErrorMsg, MsgSize, "Out of memory");
				return 4;
			}
		}

		size_t ReadBytes = fread(Context.Buffer, 1, FrameSize, Context.ST.fp);
		if (ReadBytes != FrameSize) {
			if (ReadBytes == 0) {
				if (feof(Context.ST.fp)) {
					snprintf(ErrorMsg, MsgSize, "Unexpected EOF while reading frame");
					return 5;
				} else {
					snprintf(ErrorMsg, MsgSize, "Error reading frame: %s", strerror(errno));
					return 6;
				}
			} else {
				snprintf(ErrorMsg, MsgSize, "Short read while reading frame");
				return 7;
			}
			snprintf(ErrorMsg, MsgSize, "Unknown read error");
			return 8;
		}

		return 0;
	}
}

void InitNullPacket(AVPacket *pkt) {
	av_init_packet(pkt);
	pkt->data = NULL;
	pkt->size = 0;
}

void FillAP(TAudioProperties &AP, AVCodecContext *CTX, FFTrack &Frames) {
	AP.SampleFormat = static_cast<FFMS_SampleFormat>(CTX->sample_fmt);
	AP.BitsPerSample = av_get_bits_per_sample_format(CTX->sample_fmt);
	if (CTX->sample_fmt == SAMPLE_FMT_S32)
		AP.BitsPerSample = CTX->bits_per_raw_sample;

	AP.Channels = CTX->channels;;
	AP.ChannelLayout = CTX->channel_layout;
	AP.SampleRate = CTX->sample_rate;
	AP.NumSamples = (Frames.back()).SampleStart;
	AP.FirstTime = ((Frames.front().DTS * Frames.TB.Num) / (double)Frames.TB.Den) / 1000;
	AP.LastTime = ((Frames.back().DTS * Frames.TB.Num) / (double)Frames.TB.Den) / 1000;
}

#ifdef HAALISOURCE

unsigned vtSize(VARIANT &vt) {
	if (V_VT(&vt) != (VT_ARRAY | VT_UI1))
		return 0;
	long lb,ub;
	if (FAILED(SafeArrayGetLBound(V_ARRAY(&vt),1,&lb)) ||
		FAILED(SafeArrayGetUBound(V_ARRAY(&vt),1,&ub)))
		return 0;
	return ub - lb + 1;
}

void vtCopy(VARIANT& vt,void *dest) {
	unsigned sz = vtSize(vt);
	if (sz > 0) {
		void  *vp;
		if (SUCCEEDED(SafeArrayAccessData(V_ARRAY(&vt),&vp))) {
			memcpy(dest,vp,sz);
			SafeArrayUnaccessData(V_ARRAY(&vt));
		}
	}
}

#else

// used for matroska<->ffmpeg codec ID mapping to avoid Win32 dependency
typedef struct BITMAPINFOHEADER {
        uint32_t      biSize;
        int32_t       biWidth;
        int32_t       biHeight;
        uint16_t      biPlanes;
        uint16_t      biBitCount;
        uint32_t      biCompression;
        uint32_t      biSizeImage;
        int32_t       biXPelsPerMeter;
        int32_t       biYPelsPerMeter;
        uint32_t      biClrUsed;
        uint32_t      biClrImportant;
} BITMAPINFOHEADER;

#define MAKEFOURCC(ch0, ch1, ch2, ch3)\
	((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |\
	((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24 ))

#endif

CodecID MatroskaToFFCodecID(char *Codec, void *CodecPrivate) {
/* Look up native codecs */
	for(int i = 0; ff_mkv_codec_tags[i].id != CODEC_ID_NONE; i++){
		if(!strncmp(ff_mkv_codec_tags[i].str, Codec,
			strlen(ff_mkv_codec_tags[i].str))){
				return ff_mkv_codec_tags[i].id;
			}
	}

/* Video codecs for "avi in mkv" mode */
	const AVCodecTag *const tags[] = { codec_bmp_tags, 0 };
	if (!strcmp(Codec, "V_MS/VFW/FOURCC"))
		return av_codec_get_id(tags, ((BITMAPINFOHEADER *)CodecPrivate)->biCompression);

// FIXME
/* Audio codecs for "avi in mkv" mode */
		//#include "Mmreg.h"
		//((WAVEFORMATEX *)TI->CodecPrivate)->wFormatTag

/* Fixup for uncompressed video formats */

/* Fixup for uncompressed audio formats */

	return CODEC_ID_NONE;
}
