/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / G.726 decoder filter.
 *
 *  G.726 is ITU-T's ADPCM at 16, 24, 32 and 40 kbit/s. The repository already
 *  carries Sun's public domain g72x code, through libg711, and the obvious
 *  move was to reuse it - G.726 is after all the 1990 merge of G.721 and
 *  G.723. Measured against ffmpeg's G.726 on the same bitstreams, that turns
 *  out to hold only at the low rates:
 *
 *      16 kbit/s   Sun 11.6 dB   ffmpeg 11.8 dB   against the source
 *      24 kbit/s   Sun 17.1 dB   ffmpeg 17.7 dB
 *      32 kbit/s   Sun 21.4 dB   ffmpeg 23.4 dB
 *      40 kbit/s   Sun -3.4 dB   ffmpeg 28.5 dB
 *
 *  At 40 kbit/s the Sun code does not decode a G.726 stream at all, and the
 *  reverse holds too: on a Sun .au file coded at 40 kbit/s the Sun code gives
 *  28.6 dB and ffmpeg 8.0 dB. They are two different bitstreams. G.721/G.723
 *  as Sun implemented them and G.726 as ITU-T published it are not
 *  interchangeable above 24 kbit/s, which is why this is a separate filter
 *  rather than another rate in libg711's audec.
 *
 *  Raw G.726 carries no header at all - no magic, no rate, no bit rate - so
 *  the two things a decoder has to know are options: the code word size in
 *  bits, and which end of the byte the code words start from. ITU-T and RTP
 *  pack them MSB first ("left justified", what ffmpeg writes as .g726); WAV
 *  and the Sun code pack them LSB first ("right justified", ffmpeg's g726le).
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <libavcodec/avcodec.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 srate, bits;
	Bool le;
	AVCodecContext *dec;
	AVFrame *frame;
	AVPacket *pkt;
} GF_G726DecCtx;

static GF_Err g726dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const AVCodec *codec;
	GF_G726DecCtx *ctx = (GF_G726DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	if ((ctx->bits < 2) || (ctx->bits > 5))
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[G726Dec] bits must be 2, 3, 4 or 5 (16, 24, 32 or 40 kbit/s), got %u\n", ctx->bits));
		return GF_BAD_PARAM;
	}
	if (!ctx->srate)
		return GF_BAD_PARAM;

	codec = avcodec_find_decoder(ctx->le ? AV_CODEC_ID_ADPCM_G726LE : AV_CODEC_ID_ADPCM_G726);
	if (!codec)
		return GF_NOT_SUPPORTED;
	if (ctx->dec)
		avcodec_free_context(&ctx->dec);
	ctx->dec = avcodec_alloc_context3(codec);
	if (!ctx->dec)
		return GF_OUT_OF_MEM;
	ctx->dec->sample_rate = (int)ctx->srate;
	ctx->dec->bits_per_coded_sample = (int)ctx->bits;
	av_channel_layout_default(&ctx->dec->ch_layout, 1);
	if (avcodec_open2(ctx->dec, codec, NULL) < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[G726Dec] Could not open the decoder\n"));
		return GF_NOT_SUPPORTED;
	}

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->srate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->srate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(1));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_CENTER));

	return GF_OK;
}

static GF_Err g726dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	const u8 *data;
	u8 *output;
	u32 size, pos = 0, nb_samples = 0, out_alloc, chunk;
	GF_G726DecCtx *ctx = (GF_G726DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || !size)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	/* Every byte holds 8/bits code words, so the sample count is exact. One
	 * packet out for the whole file, like the other whole-file speech
	 * decoders here. */
	out_alloc = (size * 8 / ctx->bits) * 2;
	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_alloc, &output);
	if (!dst_pck)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	/* The decoder wants whole code words in a packet; bits*8 bytes is the
	 * smallest count that is whole for every rate, so feed multiples of it. */
	chunk = ctx->bits * 8 * 10;

	while (pos < size)
	{
		u32 this_chunk = (size - pos < chunk) ? (size - pos) : chunk;
		int ret;

		this_chunk -= this_chunk % (ctx->bits * 8 / 8 ? 1 : 1);
		av_packet_unref(ctx->pkt);
		ctx->pkt->data = (u8 *)data + pos;
		ctx->pkt->size = (int)this_chunk;
		ret = avcodec_send_packet(ctx->dec, ctx->pkt);
		ctx->pkt->data = NULL;
		ctx->pkt->size = 0;
		if (ret < 0)
			break;
		pos += this_chunk;

		while (1)
		{
			u32 n;
			ret = avcodec_receive_frame(ctx->dec, ctx->frame);
			if (ret < 0)
				break;
			n = (u32)ctx->frame->nb_samples;
			if ((nb_samples + n) * 2 > out_alloc)
				break;
			memcpy(output + (size_t)nb_samples * 2, ctx->frame->data[0], (size_t)n * 2);
			nb_samples += n;
		}
	}

	if (!nb_samples)
	{
		gf_filter_pck_discard(dst_pck);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[G726Dec] Nothing decoded - check bits and le against how the file was written\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pck_truncate(dst_pck, nb_samples * 2);
	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_duration(dst_pck, nb_samples);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static GF_Err g726dec_initialize(GF_Filter *filter)
{
	GF_G726DecCtx *ctx = (GF_G726DecCtx *)gf_filter_get_udta(filter);
	ctx->frame = av_frame_alloc();
	ctx->pkt = av_packet_alloc();
	if (!ctx->frame || !ctx->pkt)
		return GF_OUT_OF_MEM;
	return GF_OK;
}

static void g726dec_finalize(GF_Filter *filter)
{
	GF_G726DecCtx *ctx = (GF_G726DecCtx *)gf_filter_get_udta(filter);
	if (ctx->frame)
		av_frame_free(&ctx->frame);
	if (ctx->pkt)
		av_packet_free(&ctx->pkt);
	if (ctx->dec)
		avcodec_free_context(&ctx->dec);
}

static const GF_FilterCapability G726DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "g726|g721|g726le"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/g726|audio/x-g726"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

#define OFFS(_n) #_n, offsetof(GF_G726DecCtx, _n)
static const GF_FilterArgs G726DecArgs[] =
	{
		/* no "|" lists in min_max_enum: that would make these enumerations and
		   resolve their defaults to list indices rather than to numbers */
		{OFFS(bits), "code word size in bits: 2, 3, 4 or 5, i.e. 16, 24, 32 or 40 kbit/s. Raw G.726 does not carry it", GF_PROP_UINT, "4", NULL, 0},
		{OFFS(srate), "sampling rate in Hz; G.726 is a narrowband telephony codec, so 8000 unless the stream says otherwise", GF_PROP_UINT, "8000", NULL, 0},
		{OFFS(le), "code words packed LSB first, as in WAV and in Sun's g72x, rather than MSB first as ITU-T and RTP pack them", GF_PROP_BOOL, "false", NULL, 0},
		{0}};

GF_FilterRegister G726DecoderRegister = {
	.name = "g726dec",
	GF_FS_SET_DESCRIPTION("G.726 ADPCM decoder")
		GF_FS_SET_HELP("This filter decodes raw ITU-T G.726 ADPCM at 16, 24, 32 or 40 kbit/s, using a reduced FFMPEG build carrying those two decoders only. A raw stream has no header, so the code word size, the sampling rate and the bit packing are options.\n"
		               "Note G.726 above 24 kbit/s is not the same bitstream as Sun's G.721 and G.723, which libg711's audec reads from .au files.")
			.private_size = sizeof(GF_G726DecCtx),
	.args = G726DecArgs,
	SETCAPS(G726DecCaps),
	.initialize = g726dec_initialize,
	.configure_pid = g726dec_configure_pid,
	.process = g726dec_process,
	.finalize = g726dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE g726dec_register(GF_FilterSession *session)
{
	return &G726DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_g726dec(void)
{
	gf_filter_auto_register("g726dec", g726dec_register);
}
