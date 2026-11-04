// xmp-ffmpeg.cpp - XMPlay input plugin, powered by FFmpeg
//
// Plays (almost) anything FFmpeg can DECODE: all audio formats, plus the audio
// stream of any video file (video itself is skipped, audio-only playback).
// Everything (libavformat/libavcodec/libswresample/libavutil + external codec
// libs) is linked STATICALLY into this DLL - no FFmpeg DLLs are required.
//
// File I/O goes through XMPlay's XMPFILE routines, so network streaming,
// archives and memory files all work automatically.
//
// Built for Windows XP+ (see .github/workflows/build.yml).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "xmpin.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define PLUGIN_NAME "xmp-ffmpeg"
// version is injected by scripts/build-plugin.sh from the VERSION file
#ifndef XMP_FFMPEG_VERSION
#define XMP_FFMPEG_VERSION "1.0.1"
#endif
#define PLUGIN_VER  XMP_FFMPEG_VERSION

static XMPFUNC_IN   *xmpfin;
static XMPFUNC_MISC *xmpfmisc;
static XMPFUNC_FILE *xmpffile;
static XMPFUNC_TEXT *xmpftext;
static DWORD xmpver;

struct FFMPEG {
	XMPFILE file;
	AVFormatContext *fmt;
	AVIOContext *io;
	AVCodecContext *dec;
	SwrContext *swr;
	AVFrame *frame;
	AVPacket *pkt;
	int stream;          // audio stream index
	double pos;          // current output position (seconds)
	double duration;     // seconds, 0 = unknown
	DWORD bitrate;       // kbps (0 = unknown)
	BOOL seekable, netfile, sent_eof, eof;
	float *outbuf;       // interleaved float32 staging buffer
	DWORD outcap;        // capacity in floats
	DWORD outlen, outp;  // floats valid / consumed
	const char *codec;   // audio codec short name
	BOOL has_video;      // playing audio from a video file
	int vw, vh;
	const char *video_codec;
};

static FFMPEG *current;

// ---------------------------------------------------------------- XMPFILE I/O

static int io_read(void *opaque, uint8_t *buf, int size)
{
	FFMPEG *f = (FFMPEG*)opaque;
	while (1) {
		DWORD n = xmpffile->Read(f->file, buf, size);
		if (n) return (int)n;
		if (!xmpffile->NetIsActive(f->file)) return AVERROR_EOF;
		if (xmpfmisc->CheckCancel()) return AVERROR_EXIT;
		if (!xmpffile->NetPreBuf(f->file)) Sleep(10); // still connected, waiting for data
	}
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
	FFMPEG *f = (FFMPEG*)opaque;
	if (whence == AVSEEK_SIZE) return (int64_t)xmpffile->GetSize64(f->file);
	int64_t base = 0;
	if (whence == SEEK_CUR) base = (int64_t)xmpffile->Tell64(f->file);
	else if (whence == SEEK_END) base = (int64_t)xmpffile->GetSize64(f->file);
	else if (whence != SEEK_SET) return -1;
	offset += base;
	if (offset < 0) return -1;
	if (xmpver >= 0x03080200) {
		if (!xmpffile->Seek64(f->file, (QWORD)offset)) return -1;
	} else {
		if (!xmpffile->Seek(f->file, (DWORD)offset)) return -1;
	}
	return offset;
}

// ------------------------------------------------------------------- helpers

static void FreeFF(FFMPEG *f)
{
	if (!f) return;
	if (f->swr) swr_free(&f->swr);
	if (f->dec) avcodec_free_context(&f->dec);
	if (f->fmt) avformat_close_input(&f->fmt); // CUSTOM_IO: does not free our AVIO
	if (f->io) {
		// avio_context_free does NOT free the IO buffer - and avio may have
		// reallocated it, so free io->buffer (per the official avio_read_callback
		// example). Without this, every file open/close leaks 64KB.
		av_freep(&f->io->buffer);
		avio_context_free(&f->io);
	}
	if (f->frame) av_frame_free(&f->frame);
	if (f->pkt) av_packet_free(&f->pkt);
	free(f->outbuf);
	free(f);
}

// bytes-per-sample code for XMPFORMAT.res: 1=8-bit 2=16-bit 3=24-bit 4=float
static WORD res_code(AVSampleFormat fmt, int rawbits)
{
	if (rawbits > 0) {
		if (rawbits <= 8) return 1;
		if (rawbits <= 16) return 2;
		if (rawbits <= 24) return 3;
		return 4;
	}
	int b = av_get_bytes_per_sample(fmt);
	if (b <= 1) return 1;
	if (b <= 2) return 2;
	if (b == 3) return 3;
	return 4;
}

// open + analyse a file (used by GetFileInfo and Open)
static FFMPEG *InitFF(const char *filename, XMPFILE file)
{
	FFMPEG *f = (FFMPEG*)calloc(1, sizeof(FFMPEG));
	if (!f) return 0;
	f->file = file;
	DWORD type = xmpffile->GetType(file);
	f->seekable = (type != XMPFILE_TYPE_NETSTREAM);
	f->netfile = (type >= XMPFILE_TYPE_NETFILE);

	const int IOBUF = 64 * 1024;
	unsigned char *iobuf = (unsigned char*)av_malloc(IOBUF);
	f->io = avio_alloc_context(iobuf, IOBUF, 0, f, io_read, NULL, f->seekable ? io_seek : NULL);
	if (!f->io) { av_free(iobuf); free(f); return 0; }
	f->io->seekable = f->seekable ? AVIO_SEEKABLE_NORMAL : 0;

	f->fmt = avformat_alloc_context();
	if (!f->fmt) goto fail;
	f->fmt->pb = f->io;
	f->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&f->fmt, filename, NULL, NULL) < 0) goto fail;
	if (avformat_find_stream_info(f->fmt, NULL) < 0) goto fail;

	{ // pick the audio stream (video streams are ignored entirely)
		int s = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (s < 0) goto fail; // no audio in this file
		f->stream = s;
	}

	{ // record video info (for display only - never decoded)
		for (unsigned i = 0; i < f->fmt->nb_streams; i++) {
			AVCodecParameters *p = f->fmt->streams[i]->codecpar;
			if (p->codec_type == AVMEDIA_TYPE_VIDEO && !(f->fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
				f->has_video = TRUE;
				f->vw = p->width; f->vh = p->height;
				const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
				f->video_codec = d ? d->name : "?";
				break;
			}
		}
	}

	{ // open the audio decoder
		AVStream *st = f->fmt->streams[f->stream];
		const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
		if (!c) goto fail;
		f->dec = avcodec_alloc_context3(c);
		if (!f->dec) goto fail;
		if (avcodec_parameters_to_context(f->dec, st->codecpar) < 0) goto fail;
		if (avcodec_open2(f->dec, c, NULL) < 0) goto fail;
		const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
		f->codec = d ? d->name : c->name;
	}

	if (f->dec->sample_rate <= 0 || f->dec->ch_layout.nb_channels <= 0) goto fail;

	// unknown channel layout -> assume default order so swresample accepts it
	if (f->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&f->dec->ch_layout, f->dec->ch_layout.nb_channels);

	// decode -> interleaved float32 at the source rate/layout (XMPlay resamples
	// and downmixes to the output device itself)
	if (swr_alloc_set_opts2(&f->swr, &f->dec->ch_layout, AV_SAMPLE_FMT_FLT, f->dec->sample_rate,
			&f->dec->ch_layout, f->dec->sample_fmt, f->dec->sample_rate, 0, NULL) < 0) goto fail;
	if (swr_init(f->swr) < 0) goto fail;

	f->frame = av_frame_alloc();
	f->pkt = av_packet_alloc();
	if (!f->frame || !f->pkt) goto fail;

	{ // length + bitrate
		AVStream *st = f->fmt->streams[f->stream];
		if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
			f->duration = st->duration * av_q2d(st->time_base);
		else if (f->fmt->duration != AV_NOPTS_VALUE && f->fmt->duration > 0)
			f->duration = (double)f->fmt->duration / AV_TIME_BASE;
		QWORD br = st->codecpar->bit_rate ? st->codecpar->bit_rate : f->fmt->bit_rate;
		f->bitrate = (DWORD)(br / 1000);
	}
	return f;
fail:
	FreeFF(f);
	return 0;
}

// tags as a flat "name\0value\0...\0\0" buffer (XMPlay convention)
static const char *tag_keys[] = { "title", "artist", "album", "date", "track", "genre", "comment" };

static char *BuildTags(FFMPEG *f)
{
	DWORD cap = 256, len = 0;
	char *buf = (char*)xmpfmisc->Alloc(cap);
	if (!buf) return 0;
	#define TAGAPPEND(s) do { \
		int n = (int)strlen(s) + 1; \
		while (len + n + 1 > cap) { cap *= 2; buf = (char*)xmpfmisc->ReAlloc(buf, cap); if (!buf) return 0; } \
		memcpy(buf + len, s, n); len += n; } while (0)

	{ // filetype first
		const AVInputFormat *iformat = f->fmt->iformat;
		TAGAPPEND("filetype");
		TAGAPPEND(iformat && iformat->long_name ? iformat->long_name : "FFmpeg");
	}
	for (int i = 0; i < (int)(sizeof(tag_keys) / sizeof(tag_keys[0])); i++) {
		AVDictionaryEntry *e = av_dict_get(f->fmt->streams[f->stream]->metadata, tag_keys[i], NULL, 0);
		if (!e) e = av_dict_get(f->fmt->metadata, tag_keys[i], NULL, 0);
		if (e && e->value && e->value[0]) {
			TAGAPPEND(tag_keys[i]);
			TAGAPPEND(e->value); // metadata is UTF-8 = XMPlay native text on NT
		}
	}
	buf[len] = 0; // terminating NULL
	#undef TAGAPPEND
	return buf;
}

static BOOL ensure_outbuf(FFMPEG *f, DWORD floats)
{
	if (floats <= f->outcap) return TRUE;
	float *p = (float*)realloc(f->outbuf, floats * sizeof(float));
	if (!p) return FALSE;
	f->outbuf = p;
	f->outcap = floats;
	return TRUE;
}

// ------------------------------------------------------------- XMPIN methods

static void WINAPI FF_About(HWND win)
{
	char msg[256];
	sprintf(msg, "%s %s\n\nPlays any audio format FFmpeg can decode,\n"
		"including the audio stream of video files.\n\nFFmpeg: %s\nXMPlay plugin interface %u",
		PLUGIN_NAME, PLUGIN_VER, av_version_info(), (unsigned)XMPIN_FACE);
	MessageBox(win, msg, "About " PLUGIN_NAME, MB_ICONINFORMATION);
}

// verify a file is ours (optional with XMPIN_FLAG_NOCHECK, kept for completeness)
static BOOL WINAPI FF_CheckFile(const char *filename, XMPFILE file)
{
	// cheap check: if it's not a known extension XMPlay may still ask; let
	// avformat probe it via GetFileInfo/Open instead of guessing here.
	return TRUE;
}

// get file info (called before Open for playlist/title display)
// return: number of subsongs (0 = not our file)
static DWORD WINAPI FF_GetFileInfo(const char *filename, XMPFILE file, float **length, char **tags)
{
	FFMPEG *f = InitFF(filename, file);
	if (!f) return 0;
	if (length && f->duration > 0) {
		float *lens = (float*)xmpfmisc->Alloc(sizeof(float));
		if (lens) {
			lens[0] = (float)f->duration;
			*length = lens;
		}
	}
	if (tags) *tags = BuildTags(f);
	FreeFF(f);
	return 1;
}

// open a file for playback
// return: 0=failed, 1=success, 2=success and XMPlay can close the file
static DWORD WINAPI FF_Open(const char *filename, XMPFILE file)
{
	current = InitFF(filename, file);
	if (!current) return 0;
	current->pos = 0;
	xmpfin->SetLength((float)(current->duration > 0 ? current->duration : -1), current->seekable);
	if (current->netfile)
		xmpffile->NetSetRate(file, current->bitrate ? current->bitrate * 125 : 32000);
	return 1;
}

static void WINAPI FF_Close()
{
	FreeFF(current);
	current = 0;
}

static char *WINAPI FF_GetTags()
{
	return current ? BuildTags(current) : NULL;
}

static void WINAPI FF_SetFormat(XMPFORMAT *form)
{
	FFMPEG *f = current;
	form->rate = f->dec->sample_rate;
	form->chan = f->dec->ch_layout.nb_channels;
	form->res = res_code(f->dec->sample_fmt, f->dec->bits_per_raw_sample);
	form->chanmask = (f->dec->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) ? (WORD)f->dec->ch_layout.u.mask : 0;
}

static void WINAPI FF_GetInfoText(char *format, char *length)
{
	FFMPEG *f = current;
	if (format) {
		format += sprintf(format, "%s", f->codec ? f->codec : "?");
		if (f->bitrate) format += sprintf(format, " - %ukb/s", (unsigned)f->bitrate);
		sprintf(format, " - %dhz", f->dec->sample_rate);
	}
	// "length" already holds the time
}

static void WINAPI FF_GetGeneralInfo(char *buf)
{
	FFMPEG *f = current;
	const AVInputFormat *iformat = f->fmt->iformat;
	buf += sprintf(buf, "Format\t%s\r", iformat && iformat->long_name ? iformat->long_name : "?");
	buf += sprintf(buf, "Audio codec\t%s\r", f->codec ? f->codec : "?");
	if (f->has_video)
		buf += sprintf(buf, "Video\t%s %dx%d (audio only)\r", f->video_codec ? f->video_codec : "?", f->vw, f->vh);
	if (f->bitrate) buf += sprintf(buf, "Bit rate\t%u kbps\r", (unsigned)f->bitrate);
	buf += sprintf(buf, "Sample rate\t%d hz\rChannels\t%d\r",
		f->dec->sample_rate, f->dec->ch_layout.nb_channels);
}

static void WINAPI FF_GetMessage(char *buf)
{
	FFMPEG *f = current;
	AVDictionary *md[2] = { f->fmt->streams[f->stream]->metadata, f->fmt->metadata };
	for (int i = 0; i < 2; i++) {
		AVDictionaryEntry *e = NULL;
		while ((e = av_dict_get(md[i], "", e, AV_DICT_IGNORE_SUFFIX)))
			buf = xmpfmisc->FormatInfoText(buf, e->key, e->value);
	}
}

// decode sample data - always interleaved floating-point
// count = number of floats to write; return number written (< count = end)
static DWORD WINAPI FF_Process(float *buffer, DWORD count)
{
	FFMPEG *f = current;
	DWORD done = 0;
	int ch = f->dec->ch_layout.nb_channels;
	if (ch < 1) ch = 1;

	while (done < count) {
		if (f->outp < f->outlen) { // staged samples available
			DWORD n = count - done;
			if (n > f->outlen - f->outp) n = f->outlen - f->outp;
			memcpy(buffer + done, f->outbuf + f->outp, n * sizeof(float));
			f->outp += n;
			done += n;
			f->pos += (double)n / ((double)ch * f->dec->sample_rate);
			continue;
		}
		if (f->eof) break;

		int ret = avcodec_receive_frame(f->dec, f->frame);
		if (ret == 0) { // got a frame -> resample to interleaved float
			int maxn = swr_get_out_samples(f->swr, f->frame->nb_samples);
			if (maxn < 1) maxn = f->frame->nb_samples + 256;
			if (!ensure_outbuf(f, (DWORD)maxn * ch)) { f->eof = TRUE; break; }
			uint8_t *dst = (uint8_t*)f->outbuf;
			int got = swr_convert(f->swr, &dst, maxn,
				(const uint8_t**)f->frame->extended_data, f->frame->nb_samples);
			if (got < 0) got = 0;
			f->outlen = (DWORD)got * ch;
			f->outp = 0;
			continue;
		}
		if (ret == AVERROR_EOF || (ret < 0 && ret != AVERROR(EAGAIN))) {
			f->eof = TRUE; // drained or decode error
			break;
		}
		// decoder needs more input
		if (f->sent_eof) { f->eof = TRUE; break; }
		ret = av_read_frame(f->fmt, f->pkt);
		if (ret >= 0) {
			if (f->pkt->stream_index != f->stream) {
				av_packet_unref(f->pkt); // video/subtitle packet - skip
				continue;
			}
			avcodec_send_packet(f->dec, f->pkt);
			av_packet_unref(f->pkt);
			continue;
		}
		if (ret == AVERROR(EAGAIN)) continue;
		f->sent_eof = TRUE;
		avcodec_send_packet(f->dec, NULL); // end of input: drain the decoder
	}
	return done;
}

// seeking granularity in seconds
static double WINAPI FF_GetGranularity()
{
	return 0.001;
}

// seek to a position (in granularity units, i.e. milliseconds)
// return the new position in seconds, or -1 if failed
static double WINAPI FF_SetPosition(DWORD pos)
{
	FFMPEG *f = current;
	double t = pos * 0.001;
	if (!f->seekable) return -1;
	if (av_seek_frame(f->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) return -1;
	avcodec_flush_buffers(f->dec);
	swr_init(f->swr); // drop any resampler delay
	f->outlen = f->outp = 0;
	f->eof = f->sent_eof = FALSE;
	f->pos = t;
	return t;
}

// ------------------------------------------------------------------ exported

extern "C" __declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
	static XMPIN xmpin = {
		XMPIN_FLAG_CANSTREAM | XMPIN_FLAG_NOCHECK | XMPIN_FLAG_MULTIEXT,
		"FFmpeg (audio)",
		"FFmpeg audio\0"
			"mp3/mp2/mp1/mpa/m4a/m4b/m4r/aac/ac3/eac3/dts/dtshd/hra/mlp/truehd/thd/"
			"flac/ogg/oga/opus/spx/wma/wav/wave/w64/wv/ape/tta/tak/ofr/ofs/mpc/mp+/mpp/"
			"vqf/8svx/svx/amr/awb/gsm/gsm610/aa3/oma/at3/at9/ra/mka/caf/snd/au/aif/aiff/"
			"aifc/voc/iff/vox/ircam/amb/dsf/dff/xwb/aax/aa/ac4/aptx\0"
		"FFmpeg video (audio only)\0"
			"mkv/mp4/avi/mov/qt/wmv/asf/flv/f4v/webm/ogv/ts/m2ts/mts/m2t/mpg/mpeg/mpe/"
			"mpv/m2v/vob/ifo/3gp/3g2/3gpp/3gp2/divx/xvid/rm/rmvb/rv/m4v/tp/trp/dvr-ms/"
			"wtv/mxf/nuv/nsv/nut/roq/swf/fla/fli/flc/bik/bk2/sm2/r3d/yop/mtv/m2v\0"
		"\0",
		FF_About,
		NULL,             // no config
		FF_CheckFile,
		FF_GetFileInfo,
		FF_Open,
		FF_Close,
		NULL,
		FF_SetFormat,
		FF_GetTags,
		FF_GetInfoText,
		FF_GetGeneralInfo,
		FF_GetMessage,
		FF_SetPosition,
		FF_GetGranularity,
		NULL,             // GetBuffering (XMPFILE I/O)
		FF_Process,
		NULL,             // WriteFile
		NULL,             // GetSamples
		NULL,             // GetSubSongs
		NULL,
		NULL,             // GetDownloaded
		// no built-in visualisation
	};

	if (face != XMPIN_FACE) {
		static int shownerror = 0;
		if (face < XMPIN_FACE && !shownerror) {
			MessageBox(0, PLUGIN_NAME " requires XMPlay 3.8 or above.", 0, MB_ICONEXCLAMATION);
			shownerror = 1;
		}
		return NULL;
	}
	xmpfin  = (XMPFUNC_IN*)faceproc(XMPFUNC_IN_FACE);
	xmpfmisc = (XMPFUNC_MISC*)faceproc(XMPFUNC_MISC_FACE);
	xmpffile = (XMPFUNC_FILE*)faceproc(XMPFUNC_FILE_FACE);
	xmpftext = (XMPFUNC_TEXT*)faceproc(XMPFUNC_TEXT_FACE);
	xmpver = xmpfmisc->GetVersion();
	return &xmpin;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hDLL);
		break;
	}
	return TRUE;
}
