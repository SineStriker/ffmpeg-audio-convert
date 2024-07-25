#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
extern "C"
}
#endif

static const AVCodec *guess_codec_from_extension(const char *filename) {
    // Guess the format from the extension
    const AVOutputFormat *fmt = av_guess_format(NULL, filename, NULL);
    if (!fmt) {
        printf("Could not guess format.\n");
        return NULL;
    }

    // Get the default audio codec for the format
    const AVCodec *codec = avcodec_find_encoder(fmt->audio_codec);
    if (!codec) {
        printf("Could not find audio codec.\n");
        return NULL;
    }

    return codec;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <input file> <sample rate> <output file>\n", argv[0]);
        return 1;
    }

    const char *input_filename = argv[1];
    int output_sample_rate = atoi(argv[2]);
    const char *output_filename = argv[3];

    AVFormatContext *input_ctx = NULL;
    AVFormatContext *output_ctx = NULL;
    AVCodecContext *input_codec_ctx = NULL;
    AVCodecContext *output_codec_ctx = NULL;
    AVStream *audio_stream = NULL;
    AVStream *out_stream = NULL;
    AVPacket pkt;
    AVFrame *frame = NULL;
    SwrContext *swr_ctx = NULL;

    if (avformat_open_input(&input_ctx, input_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open input file %s\n", input_filename);
        return 1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream info in %s\n", input_filename);
        return 1;
    }

    int audio_stream_index = -1;
    for (int i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }

    if (audio_stream_index == -1) {
        fprintf(stderr, "Could not find audio stream in %s\n", input_filename);
        return 1;
    }

    AVStream *in_stream = input_ctx->streams[audio_stream_index];
    AVCodecParameters *codec_params = in_stream->codecpar;
    const AVCodec *input_codec = avcodec_find_decoder(codec_params->codec_id);
    if (!input_codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return 1;
    }

    input_codec_ctx = avcodec_alloc_context3(input_codec);
    if (!input_codec_ctx) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        return 1;
    }

    if (avcodec_parameters_to_context(input_codec_ctx, codec_params) < 0) {
        fprintf(stderr, "Could not copy codec parameters to context\n");
        return 1;
    }

    if (avcodec_open2(input_codec_ctx, input_codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 1;
    }

    // Output context setup
    avformat_alloc_output_context2(&output_ctx, NULL, NULL, output_filename);
    if (!output_ctx) {
        fprintf(stderr, "Could not create output context\n");
        return 1;
    }

    const AVCodec *output_codec = guess_codec_from_extension(output_filename);
    if (!output_codec) {
        return 1;
    }

    out_stream = avformat_new_stream(output_ctx, output_codec);
    if (!out_stream) {
        fprintf(stderr, "Failed to allocate output stream\n");
        return 1;
    }

    output_codec_ctx = avcodec_alloc_context3(output_codec);
    if (!output_codec_ctx) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        return 1;
    }

    output_codec_ctx->sample_rate = output_sample_rate;

    // output_codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    // output_codec_ctx->channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
    av_channel_layout_copy(&output_codec_ctx->ch_layout, &input_codec_ctx->ch_layout);

    output_codec_ctx->sample_fmt = output_codec->sample_fmts[0];
    output_codec_ctx->bit_rate = 64000;

    if (avcodec_open2(output_codec_ctx, output_codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return 1;
    }

    avcodec_parameters_from_context(out_stream->codecpar, output_codec_ctx);

    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file\n");
            return 1;
        }
    }

    if (avformat_write_header(output_ctx, NULL) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return 1;
    }

    frame = av_frame_alloc();
    if (swr_alloc_set_opts2(&swr_ctx, &output_codec_ctx->ch_layout, output_codec_ctx->sample_fmt,
                            output_codec_ctx->sample_rate, &codec_params->ch_layout,
                            (enum AVSampleFormat) codec_params->format, codec_params->sample_rate,
                            0, NULL) ||
        swr_init(swr_ctx) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        return 1;
    }

    if (!swr_ctx || swr_init(swr_ctx) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        return 1;
    }

    while (av_read_frame(input_ctx, &pkt) >= 0) {
        if (pkt.stream_index == audio_stream_index) {
            if (avcodec_send_packet(input_codec_ctx, &pkt) < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                return 1;
            }

            while (avcodec_receive_frame(input_codec_ctx, frame) == 0) {
                AVFrame *resampled_frame = av_frame_alloc();
                resampled_frame->sample_rate = output_codec_ctx->sample_rate;
                resampled_frame->format = output_codec_ctx->sample_fmt;
                resampled_frame->nb_samples = frame->nb_samples;
                av_channel_layout_copy(&resampled_frame->ch_layout, &output_codec_ctx->ch_layout);

                if (av_frame_get_buffer(resampled_frame, 0) < 0) {
                    fprintf(stderr, "Could not allocate audio buffer\n");
                    return 1;
                }

                swr_convert_frame(swr_ctx, resampled_frame, frame);

                av_packet_unref(&pkt);

                if (avcodec_send_frame(output_codec_ctx, resampled_frame) < 0) {
                    fprintf(stderr, "Error sending frame to encoder\n");
                    return 1;
                }

                while (avcodec_receive_packet(output_codec_ctx, &pkt) == 0) {
                    av_packet_rescale_ts(&pkt, output_codec_ctx->time_base, out_stream->time_base);
                    pkt.stream_index = out_stream->index;
                    if (av_interleaved_write_frame(output_ctx, &pkt) < 0) {
                        fprintf(stderr, "Error while writing output packet\n");
                        return 1;
                    }
                }
                av_frame_free(&resampled_frame);
            }
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(output_ctx);

    avcodec_free_context(&input_codec_ctx);
    avcodec_free_context(&output_codec_ctx);
    avformat_close_input(&input_ctx);
    if (output_ctx && !(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output_ctx->pb);
    }
    avformat_free_context(output_ctx);
    av_frame_free(&frame);
    swr_free(&swr_ctx);

    return 0;
}
