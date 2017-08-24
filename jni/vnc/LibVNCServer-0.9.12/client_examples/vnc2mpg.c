/**
 * @example vnc2mpg.c
 * Simple movie writer for vnc; based on Libavformat API example from FFMPEG
 * 
 * Copyright (c) 2003 Fabrice Bellard, 2004 Johannes E. Schindelin
 * Updates copyright (c) 2017 Tyrel M. McQueen
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.  
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <rfb/rfbclient.h>

#define VNC_PIX_FMT       AV_PIX_FMT_RGB565  /* pixel format generated by VNC client */
#define OUTPUT_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

static int write_packet(AVFormatContext *oc, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    /* Write the compressed frame to the media file. */
    return av_interleaved_write_frame(oc, pkt);
}

/*************************************************/
/* video functions                               */

/* a wrapper around a single output video stream */
typedef struct {
    AVStream *st;
    AVCodec *codec;
    AVCodecContext *enc;
    int64_t pts;
    AVFrame *frame;
    AVFrame *tmp_frame;
    struct SwsContext *sws;
} VideoOutputStream;

/* Add an output video stream. */
int add_video_stream(VideoOutputStream *ost, AVFormatContext *oc,
                       enum AVCodecID codec_id, int64_t br, int sr, int w, int h)
{
    int i;

    /* find the encoder */
    ost->codec = avcodec_find_encoder(codec_id);
    if (!(ost->codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        return -1;
    } // no extra memory allocation from this call
    if (ost->codec->type != AVMEDIA_TYPE_VIDEO) {
	fprintf(stderr, "Encoder for '%s' does not seem to be for video.\n",
		avcodec_get_name(codec_id));
	return -2;
    }
    ost->enc = avcodec_alloc_context3(ost->codec);
    if (!(ost->enc)) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        return -3;
    } // from now on need to call avcodec_free_context(&(ost->enc)) on error

    /* Set codec parameters */
    ost->enc->codec_id = codec_id;
    ost->enc->bit_rate = br;
    /* Resolution must be a multiple of two (round up to avoid buffer overflow). */
    ost->enc->width    = w + (w % 2);
    ost->enc->height   = h + (h % 2);
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    ost->enc->time_base      = (AVRational){ 1, sr };
    ost->enc->gop_size      = 12; /* emit one intra frame every twelve frames at most */
    ost->enc->pix_fmt       = OUTPUT_PIX_FMT;
    if (ost->enc->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* Needed to avoid using macroblocks in which some coeffs overflow.
         * This does not happen with normal video, it just happens here as
         * the motion of the chroma plane does not match the luma plane. */
        ost->enc->mb_decision = 2;
    }

    ost->st = avformat_new_stream(oc, ost->codec);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        avcodec_free_context(&(ost->enc));
        return -4;
    } // stream memory cleared up when oc is freed, so no need to do so later in this function on error
    ost->st->id = oc->nb_streams-1;
    ost->st->time_base = ost->enc->time_base;
    ost->pts = 0;

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ost->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // must wait to allocate frame buffers until codec is opened (in case codec changes the PIX_FMT)
    return 0;
}

AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;
    picture = av_frame_alloc();
    if (!picture)
        return NULL;
        // from now on need to call av_frame_free(&picture) on error
    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;
    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 64);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        av_frame_free(&picture);
        return NULL;
    }
    return picture;
} // use av_frame_free(&picture) to free memory from this call

int open_video(AVFormatContext *oc, VideoOutputStream *ost)
{
    int ret;
    /* open the codec */
    ret = avcodec_open2(ost->enc, ost->codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        return ret;
    } // memory from this call freed when oc is freed, no need to do it on error in this call
    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters.\n");
        return ret;
    } // memory from this call is freed when oc (parent of ost->st) is freed, no need to do it on error in this call
    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(ost->enc->pix_fmt, ost->enc->width, ost->enc->height);
    if (!(ost->frame)) {
        fprintf(stderr, "Could not allocate video frame\n");
        return -1;
    } // from now on need to call av_frame_free(&(ost->frame)) on error
    /* If the output format is not the same as the VNC format, then a temporary VNC format
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    ost->sws = NULL;
    if (ost->enc->pix_fmt != VNC_PIX_FMT) {
        ost->tmp_frame = alloc_picture(VNC_PIX_FMT, ost->enc->width, ost->enc->height);
        if (!(ost->tmp_frame)) {
            fprintf(stderr, "Could not allocate temporary picture\n");
            av_frame_free(&(ost->frame));
            return -2;
        } // from now on need to call av_frame_free(&(ost->tmp_frame)) on error
        ost->sws = sws_getCachedContext(ost->sws, ost->enc->width, ost->enc->height, VNC_PIX_FMT, ost->enc->width, ost->enc->height, ost->enc->pix_fmt, 0, NULL, NULL, NULL);
        if (!(ost->sws)) {
            fprintf(stderr, "Could not get sws context\n");
            av_frame_free(&(ost->frame));
            av_frame_free(&(ost->tmp_frame));
            return -3;
        } // from now on need to call sws_freeContext(ost->sws); ost->sws = NULL; on error
    }

    return 0;
}

/*
 * encode current video frame and send it to the muxer
 * return 0 on success, negative on error
 */
int write_video_frame(AVFormatContext *oc, VideoOutputStream *ost, int64_t pts)
{
    int ret, ret2;
    AVPacket pkt = { 0 };
    if (pts <= ost->pts) return 0; // nothing to do
    /* convert format if needed */
    if (ost->tmp_frame) {
        sws_scale(ost->sws, (const uint8_t * const *)ost->tmp_frame->data,
                    ost->tmp_frame->linesize, 0, ost->enc->height, ost->frame->data, ost->frame->linesize);
    }

    /* send the imager to encoder */
    ost->pts = pts;
    ost->frame->pts = ost->pts;
    ret = avcodec_send_frame(ost->enc, ost->frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending video frame to encoder: %s\n", av_err2str(ret));
        return ret;
    }
    /* read all available packets */
    ret2 = 0;
    for (ret = avcodec_receive_packet(ost->enc, &pkt); ret == 0; ret = avcodec_receive_packet(ost->enc, &pkt)) {
        ret2 = write_packet(oc, &(ost->enc->time_base), ost->st, &pkt);
        if (ret2 < 0) {
            fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret2));
            /* continue on this error to not gum up encoder */
        }
    }
    if (ret2 < 0) return ret2;
    if (!(ret == AVERROR(EAGAIN))) return ret; // if AVERROR(EAGAIN), means all available packets output, need more frames (i.e. success)
    return 0;
}

/*
 * Write final video frame (i.e. drain codec).
 */
int write_final_video_frame(AVFormatContext *oc, VideoOutputStream *ost)
{
    int ret, ret2;
    AVPacket pkt = { 0 };

    /* send NULL image to encoder */
    ret = avcodec_send_frame(ost->enc, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error sending final video frame to encoder: %s\n", av_err2str(ret));
        return ret;
    }
    /* read all available packets */
    ret2 = 0;
    for (ret = avcodec_receive_packet(ost->enc, &pkt); ret == 0; ret = avcodec_receive_packet(ost->enc, &pkt)) {
        ret2 = write_packet(oc, &(ost->enc->time_base), ost->st, &pkt);
        if (ret2 < 0) {
            fprintf(stderr, "Error while writing final video frame: %s\n", av_err2str(ret2));
            /* continue on this error to not gum up encoder */
        }
    }
    if (ret2 < 0) return ret2;
    if (!(ret == AVERROR(EOF))) return ret;
    return 0;
}

void close_video_stream(VideoOutputStream *ost)
{
    avcodec_free_context(&(ost->enc));
    av_frame_free(&(ost->frame));
    av_frame_free(&(ost->tmp_frame));
    sws_freeContext(ost->sws); ost->sws = NULL;
    ost->codec = NULL; /* codec not an allocated item */
    ost->st = NULL; /* freeing parent oc will free this memory */
}

/**************************************************************/
/* Output movie handling */
AVFormatContext *movie_open(char *filename, VideoOutputStream *video_st, int br, int fr, int w, int h) {
    int ret;
    AVFormatContext *oc;

    /* allocate the output media context. */
    ret = avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if (ret < 0) {
        fprintf(stderr, "Warning: Could not deduce output format from file extension: using MP4.\n");
        ret = avformat_alloc_output_context2(&oc, NULL, "mp4", filename);
    }
    if (ret < 0) {
        fprintf(stderr, "Error: Could not allocate media context: %s.\n", av_err2str(ret));
        return NULL;
    } // from now on, need to call avformat_free_context(oc); oc=NULL; to free memory on error

    /* Add the video stream using the default format codec and initialize the codec. */
    if (oc->oformat->video_codec != AV_CODEC_ID_NONE) {
        ret = add_video_stream(video_st, oc, oc->oformat->video_codec, br, fr, w, h);
    } else {
	ret = -1;
    }
    if (ret < 0) {
        fprintf(stderr, "Error: chosen output format does not have a video codec, or error %i\n", ret);
        avformat_free_context(oc); oc = NULL;
        return NULL;
    } // from now on, need to call close_video_stream(video_st) to free memory on error

    /* Now that all the parameters are set, we can open the codecs and allocate the necessary encode buffers. */
    ret = open_video(oc, video_st); 
    if (ret < 0) {
        fprintf(stderr, "Error: error opening video codec, error %i\n", ret);
        close_video_stream(video_st);
        avformat_free_context(oc); oc = NULL;
        return NULL;
    } // no additional calls required to free memory, as close_video_stream(video_st) will do it

    /* open the output file, if needed */
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s': %s\n", filename,
                    av_err2str(ret));
            close_video_stream(video_st);
            avformat_free_context(oc); oc = NULL;
            return NULL;
        }
    } // will need to call avio_closep(&oc->pb) to free file handle on error

    /* Write the stream header, if any. */
    ret = avformat_write_header(oc, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when writing to output file: %s\n",
                av_err2str(ret));
        if (!(oc->oformat->flags & AVFMT_NOFILE))
	    avio_closep(&oc->pb);
	close_video_stream(video_st);
	avformat_free_context(oc); oc = NULL;
    } // no additional items to free

    return oc;
}

void movie_close(AVFormatContext **ocp, VideoOutputStream *video_st) {
    AVFormatContext *oc = *ocp;
    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    if (oc) {
        if (video_st)
            write_final_video_frame(oc, video_st);

        av_write_trailer(oc);

        /* Close the video codec. */
        close_video_stream(video_st);

        if (!(oc->oformat->flags & AVFMT_NOFILE))
            /* Close the output file. */
            avio_closep(&oc->pb);

        /* free the stream */
        avformat_free_context(oc);
        ocp = NULL;
    }
}

/**************************************************************/
/* VNC globals */
VideoOutputStream video_st = { 0 };
rfbClient *client = NULL;
rfbBool quit = FALSE;
char *filename = NULL;
AVFormatContext *oc = NULL;
int bitrate = 1000000;
int framerate = 5;
long max_time = 0;
struct timespec start_time, cur_time;

/* Signal handling */
void signal_handler(int signal) {
        quit=TRUE;
}

/* returns time since start in pts units */
int64_t time_to_pts(int framerate, struct timespec *start_time, struct timespec *cur_time) {
    time_t ds = cur_time->tv_sec - start_time->tv_sec;
    long dns = cur_time->tv_nsec - start_time->tv_nsec;
    /* use usecs */
    int64_t dt = (int64_t)ds*(int64_t)1000000+(int64_t)dns/(int64_t)1000;
    /* compute rv in units of frame number (rounding to nearest, not truncating) */
    int64_t rv = (((int64_t)framerate)*dt + (int64_t)500000) / (int64_t)(1000000);

    return rv;
}

/* VNC callback functions */
rfbBool vnc_malloc_fb(rfbClient* client) {
        movie_close(&oc, &video_st);
        oc = movie_open(filename, &video_st, bitrate, framerate, client->width, client->height);
	if (!oc) 
	    return FALSE;
        signal(SIGINT,signal_handler);
        signal(SIGTERM,signal_handler);
        signal(SIGQUIT,signal_handler);
        signal(SIGABRT,signal_handler);
        /* These assignments assumes the AVFrame buffer is contigous. This is true in current ffmpeg versions for
         * most non-HW accelerated bits, but may not be true globally. */
        if(video_st.tmp_frame)
               	client->frameBuffer=video_st.tmp_frame->data[0];
        else
            	client->frameBuffer=video_st.frame->data[0];
        return TRUE;
}

void vnc_update(rfbClient* client,int x,int y,int w,int h) {
}

/**************************************************************/
/* media file output */
int main(int argc, char **argv)
{
    int i,j;

    /* Initialize vnc client structure (don't connect yet). */
    client = rfbGetClient(5,3,2);
    client->format.redShift=11; client->format.redMax=31;
    client->format.greenShift=5; client->format.greenMax=63;
    client->format.blueShift=0; client->format.blueMax=31;

    /* Initialize libavcodec, and register all codecs and formats. */
    av_register_all();

    /* Parse command line. */
    for(i=1;i<argc;i++) {
            j=i;
            if(argc>i+1 && !strcmp("-o",argv[i])) {
                    filename=argv[i+1];
                    j+=2;
            } else if(argc>i+1 && !strcmp("-t",argv[i])) {
                    max_time=atol(argv[i+1]);
		    if (max_time < 10 || max_time > 100000000) {
			fprintf(stderr, "Warning: Nonsensical time-per-file %li, resetting to default.\n", max_time);
			max_time = 0;
		    }
                    j+=2;
            }
            /* This is so that argc/argv are ready for passing to rfbInitClient */
            if(j>i) {
                    argc-=j-i;
                    memmove(argv+i,argv+j,(argc-i)*sizeof(char*));
                    i--;
            }
    }

    /* default filename. */
    if (!filename) {
        fprintf(stderr, "Warning: No filename specified. Using output.mp4\n");
        filename = "output.mp4";
    }

    /* open VNC connection. */
    client->MallocFrameBuffer=vnc_malloc_fb;
    client->GotFrameBufferUpdate=vnc_update;
    if(!rfbInitClient(client,&argc,argv)) {
        printf("usage: %s [-o output_file] [-t seconds-per-file] server:port\n", argv[0]);
        return 1;
    }

    /* main loop */
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    while(!quit) {
        int i=WaitForMessage(client,10000/framerate); /* useful for timeout to be no more than 10 msec per second (=10000/framerate usec) */
	if (i>0) {
            if(!HandleRFBServerMessage(client))
                quit=TRUE;
        } else if (i<0) {
            quit=TRUE;
	}
        if (!quit) {
            clock_gettime(CLOCK_MONOTONIC, &cur_time);
            write_video_frame(oc, &video_st, time_to_pts(framerate, &start_time, &cur_time));
            if ((cur_time.tv_sec - start_time.tv_sec) > max_time && max_time > 0) {
		quit = TRUE;
            }
        }
    }
    movie_close(&oc,&video_st);
    return 0;
}
