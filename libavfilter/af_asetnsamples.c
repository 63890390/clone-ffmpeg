/*
 * Copyright (c) 2012 Andrey Utkin
 * Copyright (c) 2012 Stefano Sabatini
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Filter that changes number of samples on single output operation
 */

#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "internal.h"
#include "formats.h"

typedef struct {
    const AVClass *class;
    int nb_out_samples;  ///< how many samples to output
    AVAudioFifo *fifo;   ///< samples are queued here
    int64_t next_out_pts;
    int req_fullfilled;
    int pad;
} ASNSContext;

#define OFFSET(x) offsetof(ASNSContext, x)

static const AVOption asetnsamples_options[] = {
{ "pad", "pad last frame with zeros", OFFSET(pad), AV_OPT_TYPE_INT, {.dbl=1}, 0, 1 },
{ "p",   "pad last frame with zeros", OFFSET(pad), AV_OPT_TYPE_INT, {.dbl=1}, 0, 1 },
{ "nb_out_samples", "set the number of per-frame output samples", OFFSET(nb_out_samples), AV_OPT_TYPE_INT, {.dbl=1024}, 1, INT_MAX },
{ "n",              "set the number of per-frame output samples", OFFSET(nb_out_samples), AV_OPT_TYPE_INT, {.dbl=1024}, 1, INT_MAX },
{ NULL }
};

AVFILTER_DEFINE_CLASS(asetnsamples);

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ASNSContext *asns = ctx->priv;
    int err;

    asns->class = &asetnsamples_class;
    av_opt_set_defaults(asns);

    if ((err = av_set_options_string(asns, args, "=", ":")) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing options string: '%s'\n", args);
        return err;
    }

    asns->next_out_pts = AV_NOPTS_VALUE;
    av_log(ctx, AV_LOG_INFO, "nb_out_samples:%d pad:%d\n", asns->nb_out_samples, asns->pad);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ASNSContext *asns = ctx->priv;
    av_audio_fifo_free(asns->fifo);
}

static int config_props_output(AVFilterLink *outlink)
{
    ASNSContext *asns = outlink->src->priv;
    int nb_channels = av_get_channel_layout_nb_channels(outlink->channel_layout);

    asns->fifo = av_audio_fifo_alloc(outlink->format, nb_channels, asns->nb_out_samples);
    if (!asns->fifo)
        return AVERROR(ENOMEM);

    return 0;
}

static int push_samples(AVFilterLink *outlink)
{
    ASNSContext *asns = outlink->src->priv;
    AVFilterBufferRef *outsamples = NULL;
    int nb_out_samples, nb_pad_samples;

    if (asns->pad) {
        nb_out_samples = av_audio_fifo_size(asns->fifo) ? asns->nb_out_samples : 0;
        nb_pad_samples = nb_out_samples - FFMIN(nb_out_samples, av_audio_fifo_size(asns->fifo));
    } else {
        nb_out_samples = FFMIN(asns->nb_out_samples, av_audio_fifo_size(asns->fifo));
        nb_pad_samples = 0;
    }

    if (!nb_out_samples)
        return 0;

    outsamples = ff_get_audio_buffer(outlink, AV_PERM_WRITE, nb_out_samples);
    av_assert0(outsamples);

    av_audio_fifo_read(asns->fifo,
                       (void **)outsamples->extended_data, nb_out_samples);

    if (nb_pad_samples)
        av_samples_set_silence(outsamples->extended_data, nb_out_samples - nb_pad_samples,
                               nb_pad_samples, av_get_channel_layout_nb_channels(outlink->channel_layout),
                               outlink->format);
    outsamples->audio->nb_samples     = nb_out_samples;
    outsamples->audio->channel_layout = outlink->channel_layout;
    outsamples->audio->sample_rate    = outlink->sample_rate;
    outsamples->pts = asns->next_out_pts;

    if (asns->next_out_pts != AV_NOPTS_VALUE)
        asns->next_out_pts += nb_out_samples;

    ff_filter_samples(outlink, outsamples);
    asns->req_fullfilled = 1;
    return nb_out_samples;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamples)
{
    AVFilterContext *ctx = inlink->dst;
    ASNSContext *asns = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;
    int nb_samples = insamples->audio->nb_samples;

    if (av_audio_fifo_space(asns->fifo) < nb_samples) {
        av_log(ctx, AV_LOG_DEBUG, "No space for %d samples, stretching audio fifo\n", nb_samples);
        ret = av_audio_fifo_realloc(asns->fifo, av_audio_fifo_size(asns->fifo) + nb_samples);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Stretching audio fifo failed, discarded %d samples\n", nb_samples);
            return;
        }
    }
    av_audio_fifo_write(asns->fifo, (void **)insamples->extended_data, nb_samples);
    if (asns->next_out_pts == AV_NOPTS_VALUE)
        asns->next_out_pts = insamples->pts;
    avfilter_unref_buffer(insamples);

    if (av_audio_fifo_size(asns->fifo) >= asns->nb_out_samples)
        push_samples(outlink);
}

static int request_frame(AVFilterLink *outlink)
{
    ASNSContext *asns = outlink->src->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    int ret;

    asns->req_fullfilled = 0;
    do {
        ret = ff_request_frame(inlink);
    } while (!asns->req_fullfilled && ret >= 0);

    if (ret == AVERROR_EOF)
        while (push_samples(outlink))
            ;

    return ret;
}

AVFilter avfilter_af_asetnsamples = {
    .name           = "asetnsamples",
    .description    = NULL_IF_CONFIG_SMALL("Set the number of samples for each output audio frames."),
    .priv_size      = sizeof(ASNSContext),
    .init           = init,
    .uninit         = uninit,

    .inputs  = (const AVFilterPad[]) {
        {
            .name           = "default",
            .type           = AVMEDIA_TYPE_AUDIO,
            .filter_samples = filter_samples,
            .min_perms      = AV_PERM_READ|AV_PERM_WRITE
        },
        { .name = NULL }
    },

    .outputs = (const AVFilterPad[]) {
        {
            .name           = "default",
            .type           = AVMEDIA_TYPE_AUDIO,
            .request_frame  = request_frame,
            .config_props   = config_props_output,
        },
        { .name = NULL }
    },
};
