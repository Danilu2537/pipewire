/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <sys/socket.h>
#include <arpa/inet.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/rtp.h>
#include <module-rtp/stream.h>
#include <module-rtp/apple-midi.h>

#define BUFFER_SIZE			(1u<<22)
#define BUFFER_MASK			(BUFFER_SIZE-1)
#define BUFFER_SIZE2			(BUFFER_SIZE>>1)
#define BUFFER_MASK2			(BUFFER_SIZE2-1)

#define rtp_stream_emit(s,m,v,...)		spa_hook_list_call(&s->listener_list, \
							struct rtp_stream_events, m, v, ##__VA_ARGS__)
#define rtp_stream_emit_destroy(s)		rtp_stream_emit(s, destroy, 0)
#define rtp_stream_emit_state_changed(s,n,e)	rtp_stream_emit(s, state_changed,0,n,e)
#define rtp_stream_emit_send_packet(s,i,l)	rtp_stream_emit(s, send_packet,0,i,l)

struct impl {
	struct spa_audio_info info;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct pw_stream_events stream_events;

	struct spa_hook_list listener_list;
	struct spa_hook listener;

	const struct format_info *format_info;

	uint32_t rate;
	uint32_t stride;
	uint8_t payload;
	uint32_t ssrc;
	uint16_t seq;
	unsigned have_ssrc:1;
	unsigned have_seq:1;
	uint32_t ts_offset;
	uint32_t psamples;
	uint32_t mtu;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	struct spa_io_rate_match *io_rate_match;
	struct spa_io_position *io_position;
	struct spa_dll dll;
	double corr;
	uint32_t target_buffer;
	float max_error;

	float last_timestamp;
	float last_time;

	unsigned direct_timestamp:1;
	unsigned always_process:1;
	unsigned started:1;
	unsigned have_sync:1;
	unsigned receiving:1;
	unsigned first:1;

	int (*receive_rtp)(struct impl *impl, uint8_t *buffer, ssize_t len);
};

#include "module-rtp/audio.c"
#include "module-rtp/midi.c"

struct format_info {
	uint32_t media_subtype;
	uint32_t format;
	uint32_t size;
	const char *mime;
	const char *media_type;
};

static const struct format_info audio_format_info[] = {
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_U8, 1, "L8", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ALAW, 1, "PCMA", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ULAW, 1, "PCMU", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S16_BE, 2, "L16", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S24_BE, 3, "L24", "audio" },
	{ SPA_MEDIA_SUBTYPE_control, 0, 1, "rtp-midi", "audio" },
};

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_RateMatch:
		impl->io_rate_match = area;
		break;
	case SPA_IO_Position:
		impl->io_position = area;
		break;
	}
}

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static int stream_start(struct impl *impl)
{
	if (impl->started)
		return 0;

	rtp_stream_emit_state_changed(impl, true, NULL);

	impl->started = true;
	return 0;
}

static int stream_stop(struct impl *impl)
{
	if (!impl->started)
		return 0;

	rtp_stream_emit_state_changed(impl, false, NULL);

	impl->started = false;
	return 0;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;

	switch (state) {
		case PW_STREAM_STATE_UNCONNECTED:
			pw_log_info("stream disconnected");
			break;
		case PW_STREAM_STATE_ERROR:
			pw_log_error("stream error: %s", error);
			rtp_stream_emit_state_changed(impl, false, error);
			break;
		case PW_STREAM_STATE_STREAMING:
			if ((errno = -stream_start(impl)) < 0)
				pw_log_error("failed to start RTP stream: %m");
			break;
		case PW_STREAM_STATE_PAUSED:
			if (!impl->always_process)
				stream_stop(impl);
			impl->have_sync = false;
			break;
		default:
			break;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.io_changed = stream_io_changed,
};

static const struct format_info *find_audio_format_info(const struct spa_audio_info *info)
{
	SPA_FOR_EACH_ELEMENT_VAR(audio_format_info, f)
		if (f->media_subtype == info->media_subtype &&
		    (f->format == 0 || f->format == info->info.raw.format))
			return f;
	return NULL;
}

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static uint32_t msec_to_samples(struct impl *impl, uint32_t msec)
{
	return msec * impl->rate / 1000;
}

struct rtp_stream *rtp_stream_new(struct pw_core *core,
		enum pw_direction direction, struct pw_properties *props,
		const struct rtp_stream_events *events, void *data)
{
	struct impl *impl;
	const char *str;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	uint32_t n_params, min_samples, max_samples;
	float min_ptime, max_ptime;
	const struct spa_pod *params[1];
	enum pw_stream_flags flags;
	int latency_msec;
	int res;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		res = -errno;
		goto out;
		return NULL;
	}
	impl->first = true;
	spa_hook_list_init(&impl->listener_list);
	impl->stream_events = stream_events;

	if ((str = pw_properties_get(props, "sess.media")) == NULL)
		str = "audio";

	if (spa_streq(str, "audio")) {
		impl->info.media_type = SPA_MEDIA_TYPE_audio;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		impl->payload = 127;
	}
	else if (spa_streq(str, "midi")) {
		impl->info.media_type = SPA_MEDIA_TYPE_application;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_control;
		impl->payload = 0x61;
	}
	else {
		pw_log_error("unsupported media type:%s", str);
		res = -EINVAL;
		goto out;
	}

	switch (impl->info.media_type) {
	case SPA_MEDIA_TYPE_audio:
		parse_audio_info(props, &impl->info.info.raw);
		impl->format_info = find_audio_format_info(&impl->info);
		if (impl->format_info == NULL) {
			pw_log_error("unsupported audio format:%d channels:%d",
					impl->info.info.raw.format, impl->info.info.raw.channels);
			res = -EINVAL;
			goto out;
		}
		impl->stride = impl->format_info->size * impl->info.info.raw.channels;
		impl->rate = impl->info.info.raw.rate;
		break;
	case SPA_MEDIA_TYPE_application:
		impl->format_info = find_audio_format_info(&impl->info);
		if (impl->format_info == NULL) {
			res = -EINVAL;
			goto out;
		}
		pw_properties_set(props, PW_KEY_FORMAT_DSP, "8 bit raw midi");
		impl->stride = impl->format_info->size;
		impl->rate = pw_properties_get_uint32(props, "midi.rate", 10000);
		if (impl->rate == 0)
			impl->rate = 10000;
		break;
	default:
		spa_assert_not_reached();
		break;
	}

	pw_properties_setf(props, "rtp.mime", "%s", impl->format_info->mime);

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");

	impl->direct_timestamp = pw_properties_get_bool(props, "sess.ts-direct", false);

	if (direction == PW_DIRECTION_INPUT) {
		impl->ssrc = pw_properties_get_uint32(props, "rtp.sender-ssrc", pw_rand32());
		impl->ts_offset = pw_properties_get_uint32(props, "rtp.sender-ts-offset", pw_rand32());
	} else {
		impl->have_ssrc = pw_properties_fetch_uint32(props, "rtp.receiver-ssrc", &impl->ssrc);
		if (pw_properties_fetch_uint32(props, "rtp.receiver-ts-offset", &impl->ts_offset) < 0)
			impl->direct_timestamp = false;
	}

	impl->payload = pw_properties_get_uint32(props, "rtp.payload", impl->payload);
	impl->mtu = pw_properties_get_uint32(props, "net.mtu", DEFAULT_MTU);

	impl->seq = pw_rand32();

	str = pw_properties_get(props, "sess.min-ptime");
	if (!spa_atof(str, &min_ptime))
		min_ptime = DEFAULT_MIN_PTIME;
	str = pw_properties_get(props, "sess.max-ptime");
	if (!spa_atof(str, &max_ptime))
		max_ptime = DEFAULT_MAX_PTIME;

	min_samples = min_ptime * impl->rate / 1000;
	max_samples = max_ptime * impl->rate / 1000;

	impl->psamples = impl->mtu / impl->stride;
	impl->psamples = SPA_CLAMP(impl->psamples, min_samples, max_samples);

	latency_msec = pw_properties_get_uint32(props,
			"sess.latency.msec", DEFAULT_SESS_LATENCY);
	impl->target_buffer = msec_to_samples(impl, latency_msec);
	impl->max_error = msec_to_samples(impl, ERROR_MSEC);

	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", impl->rate);
	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
			impl->target_buffer / 2, impl->rate);

	pw_properties_setf(props, "net.mtu", "%u", impl->mtu);
	pw_properties_setf(props, "rtp.ptime", "%u",
			impl->psamples * 1000 / impl->rate);
	pw_properties_setf(props, "rtp.media", "%s", impl->format_info->media_type);
	pw_properties_setf(props, "rtp.mime", "%s", impl->format_info->mime);
	pw_properties_setf(props, "rtp.payload", "%u", impl->payload);
	pw_properties_setf(props, "rtp.rate", "%u", impl->rate);
	if (impl->info.info.raw.channels > 0)
		pw_properties_setf(props, "rtp.channels", "%u", impl->info.info.raw.channels);
	if ((str = pw_properties_get(props, "sess.ts-refclk")) != NULL) {
		pw_properties_setf(props, "rtp.ts-offset", "%u", impl->ts_offset);
		pw_properties_set(props, "rtp.ts-refclk", str);
	}

	spa_dll_init(&impl->dll);
	spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
	impl->corr = 1.0;

	impl->stream = pw_stream_new(core, "rtp-session", props);
	props = NULL;
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto out;
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;

	switch (impl->info.media_type) {
	case SPA_MEDIA_TYPE_audio:
		params[n_params++] = spa_format_audio_build(&b,
				SPA_PARAM_EnumFormat, &impl->info);
		if (direction == SPA_DIRECTION_INPUT)
			impl->stream_events.process = process_audio_capture;
		else
			impl->stream_events.process = process_audio_playback;
		impl->receive_rtp = receive_rtp_audio;
		flags |= PW_STREAM_FLAG_AUTOCONNECT;
		break;
	case SPA_MEDIA_TYPE_application:
		params[n_params++] = spa_pod_builder_add_object(&b,
                                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                SPA_FORMAT_mediaType,           SPA_POD_Id(SPA_MEDIA_TYPE_application),
                                SPA_FORMAT_mediaSubtype,        SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		if (direction == SPA_DIRECTION_INPUT)
			impl->stream_events.process = process_midi_capture;
		else
			impl->stream_events.process = process_midi_playback;
		impl->receive_rtp = receive_rtp_midi;
		break;
	default:
		res = -EINVAL;
		goto out;
	}

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&impl->stream_events, impl);

	if ((res = pw_stream_connect(impl->stream,
			direction,
			PW_ID_ANY,
			flags,
			params, n_params)) < 0) {
		pw_log_error("can't connect stream: %s", spa_strerror(res));
		goto out;
	}

	if (impl->always_process &&
		(res = stream_start(impl)) < 0)
		goto out;

	spa_hook_list_append(&impl->listener_list, &impl->listener, events, data);

	return (struct rtp_stream*)impl;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

void rtp_stream_destroy(struct rtp_stream *s)
{
	struct impl *impl = (struct impl*)s;

	rtp_stream_emit_destroy(impl);

	if (impl->stream)
		pw_stream_destroy(impl->stream);

	spa_hook_list_clean(&impl->listener_list);
	free(impl);
}

int rtp_stream_receive_packet(struct rtp_stream *s, uint8_t *buffer, size_t len)
{
	struct impl *impl = (struct impl*)s;
	return impl->receive_rtp(impl, buffer, len);
}

uint64_t rtp_stream_get_time(struct rtp_stream *s, uint64_t *rate)
{
	struct impl *impl = (struct impl*)s;
	struct spa_io_position *pos = impl->io_position;

	if (pos == NULL)
		return -EIO;

	*rate = impl->rate;
	return pos->clock.position * impl->rate *
		pos->clock.rate.num / pos->clock.rate.denom;
}
