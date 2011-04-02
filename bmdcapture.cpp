/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
** Copyright (c) 2011 Luca Barbato
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "DeckLinkAPI.h"
#include "Capture.h"
extern "C" {
#include "libavformat/avformat.h"
}

pthread_mutex_t                    sleepMutex;
pthread_cond_t                    sleepCond;
int                                videoOutputFile = -1;
int                                audioOutputFile = -1;

IDeckLink                         *deckLink;
IDeckLinkInput                    *deckLinkInput;
IDeckLinkDisplayModeIterator      *displayModeIterator;
IDeckLinkDisplayMode              *displayMode;
IDeckLinkConfiguration		  *deckLinkConfiguration;

static int                        g_videoModeIndex = -1;
static int                        g_audioChannels = 2;
static int                        g_audioSampleDepth = 16;
const char *                    g_videoOutputFile = NULL;
const char *                    g_audioOutputFile = NULL;
static int                        g_maxFrames = -1;

static unsigned long             frameCount = 0;
typedef struct AVPacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AVPacketQueue;


static AVPacketQueue queue;

static AVPacket flush_pkt;

static void avpacket_queue_init(AVPacketQueue *q)
{
    memset(q, 0, sizeof(AVPacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void avpacket_queue_flush(AVPacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void avpacket_queue_end(AVPacketQueue *q)
{
    avpacket_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (pkt!=&flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt)

        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for(;;) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            if (pkt1->pkt.data == flush_pkt.data) {
                ret = 0;
                break;
            }
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

AVFrame *picture;
AVOutputFormat *fmt = NULL;
AVFormatContext *oc;
AVStream *audio_st, *video_st;
BMDTimeValue frameRateDuration, frameRateScale;


static AVStream *add_audio_stream(AVFormatContext *oc, enum CodecID codec_id)
{
    AVCodecContext *c;
    AVCodec *codec;
    AVStream *st;

    st = av_new_stream(oc, 1);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c = st->codec;
    c->codec_id = codec_id;
    c->codec_type = AVMEDIA_TYPE_AUDIO;

    /* put sample parameters */
    c->sample_fmt = SAMPLE_FMT_S16;
//    c->bit_rate = 64000;
    c->sample_rate = 48000;
    c->channels = 2;
    // some formats want stream headers to be separate
    if(oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    codec = avcodec_find_encoder(c->codec_id);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }

    if (avcodec_open(c, codec) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc, enum CodecID codec_id)
{
    AVCodecContext *c;
    AVCodec *codec;
    AVStream *st;

    st = av_new_stream(oc, 0);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c = st->codec;
    c->codec_id = codec_id;
    c->codec_type = AVMEDIA_TYPE_VIDEO;

    /* put sample parameters */
//    c->bit_rate = 400000;
    /* resolution must be a multiple of two */
    c->width = displayMode->GetWidth();
    c->height = displayMode->GetHeight();
    /* time base: this is the fundamental unit of time (in seconds) in terms
       of which frame timestamps are represented. for fixed-fps content,
       timebase should be 1/framerate and timestamp increments should be
       identically 1.*/
    displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
    c->time_base.den = frameRateScale;
    c->time_base.num = frameRateDuration;
    c->pix_fmt = PIX_FMT_UYVY422;

    // some formats want stream headers to be separate
    if(oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /* find the video encoder */
    codec = avcodec_find_encoder(c->codec_id);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }

    /* open the codec */
    if (avcodec_open(c, codec) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }
    picture = avcodec_alloc_frame();

    return st;
}

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
    pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
    pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
    pthread_mutex_lock(&m_mutex);
        m_refCount++;
    pthread_mutex_unlock(&m_mutex);

    return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
    pthread_mutex_lock(&m_mutex);
        m_refCount--;
    pthread_mutex_unlock(&m_mutex);

    if (m_refCount == 0)
    {
        delete this;
        return 0;
    }

    return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
    void *frameBytes;
    void *audioFrameBytes;
    BMDTimeValue frameTime;
    BMDTimeValue frameDuration;

    frameCount++;
    // Handle Video Frame
    if(videoFrame)
    {
        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
        {
            fprintf(stderr, "Frame received (#%lu) - No input signal detected\n", frameCount);
            return S_OK;
        } else {
            AVPacket pkt;
            AVCodecContext *c;
            av_init_packet(&pkt);
            c = video_st->codec;
            //fprintf(stderr, "Frame received (#%lu) - Valid Frame (Size: %li bytes)\n", frameCount, videoFrame->GetRowBytes() * videoFrame->GetHeight());
            videoFrame->GetBytes(&frameBytes);
            avpicture_fill((AVPicture*)picture, (uint8_t *)frameBytes,
                           PIX_FMT_UYVY422,
                           videoFrame->GetWidth(), videoFrame->GetHeight());
            videoFrame->GetStreamTime(&frameTime, &frameDuration,
                                      video_st->time_base.den);
            pkt.pts = pkt.dts = frameTime/video_st->time_base.num;
            pkt.duration = frameDuration;
            //To be made sure it still applies
            pkt.flags |= AV_PKT_FLAG_KEY;
            pkt.stream_index= video_st->index;
            pkt.data= (uint8_t *)frameBytes;
            pkt.size= videoFrame->GetRowBytes() * videoFrame->GetHeight();
	    //fprintf(stderr,"Video Frame size %d ts %d\n", pkt.size, pkt.pts);
	    c->frame_number++;
//            av_interleaved_write_frame(oc, &pkt);
            avpacket_queue_put(&queue, &pkt);

            //write(videoOutputFile, frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
        }
//        frameCount++;

        if (g_maxFrames > 0 && frameCount >= g_maxFrames)
        {
            pthread_cond_signal(&sleepCond);
        }
    }

    // Handle Audio Frame
    if (audioFrame)
    {
            AVCodecContext *c;
            AVPacket pkt;
	    BMDTimeValue audio_pts;
            av_init_packet(&pkt);

            c = audio_st->codec;
            //hack among hacks
            pkt.size =  audioFrame->GetSampleFrameCount() *
                             g_audioChannels * (g_audioSampleDepth / 8);
            audioFrame->GetBytes(&audioFrameBytes);
            audioFrame->GetPacketTime(&audio_pts, audio_st->time_base.den);
	    pkt.dts = pkt.pts= audio_pts/audio_st->time_base.num;
	    //fprintf(stderr,"Audio Frame size %d ts %d\n", pkt.size, pkt.pts);
            pkt.flags |= AV_PKT_FLAG_KEY;
            pkt.stream_index= audio_st->index;
            pkt.data = (uint8_t *)audioFrameBytes;
//            pkt.size= avcodec_encode_audio(c, audio_outbuf, audio_outbuf_size, samples);
	    c->frame_number++;
            //write(audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() * g_audioChannels * (g_audioSampleDepth / 8));
/*            if (av_interleaved_write_frame(oc, &pkt) != 0) {
                fprintf(stderr, "Error while writing audio frame\n");
                exit(1);
            } */
            avpacket_queue_put(&queue, &pkt);

    }
    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

int usage(int status)
{
    HRESULT result;
    int displayModeCount = 0;

    fprintf(stderr,
        "Usage: Capture -m <mode id> [OPTIONS]\n"
        "\n"
        "    -m <mode id>:\n"
    );

    if (displayModeIterator)
    {
        // we try to print out some useful information about the chosen
        // card, but this only works if a card has been selected successfully

        while (displayModeIterator->Next(&displayMode) == S_OK)
        {
            char *          displayModeString = NULL;

            result = displayMode->GetName((const char **) &displayModeString);
            if (result == S_OK)
            {
                BMDTimeValue frameRateDuration, frameRateScale;
                displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
                fprintf(stderr, "        %2d:  %-20s \t %li x %li \t %g FPS\n",
                    displayModeCount, displayModeString, displayMode->GetWidth(), displayMode->GetHeight(), (double)frameRateScale / (double)frameRateDuration);
                free(displayModeString);
                displayModeCount++;
            }

            // Release the IDeckLinkDisplayMode object to prevent a leak
            displayMode->Release();
        }
    }

    fprintf(stderr,
        "    -f <filename>        Filename raw video will be written to\n"
        "    -F <format>          Define the file format to be used\n"
        "    -c <channels>        Audio Channels (2, 8 or 16 - default is 2)\n"
        "    -s <depth>           Audio Sample Depth (16 or 32 - default is 16)\n"
        "    -n <frames>          Number of frames to capture (default is unlimited)\n"
        "    -I <input>           Input connection:\n"
        "                         1: Composite video + analog audio\n"
        "                         2: Components video + analog audio\n"
        "                         3: HDMI video + audio\n"
        "                         4: SDI video + audio\n"
        "\n"
        "Capture video and/or audio to a file. Raw video and/or audio can be viewed with mplayer eg:\n"
        "\n"
        "    Capture -m2 -n 50 -f video.raw -a audio.raw\n"
        "    mplayer video.raw -demuxer rawvideo -rawvideo pal:uyvy -audiofile audio.raw -audio-demuxer 20 -rawaudio rate=48000\n"
    );

    exit(status);
}

static void *push_packet(void *ctx)
{
    AVFormatContext *s = (AVFormatContext *)ctx;
    AVPacket pkt;
    int ret;

    while (avpacket_queue_get(&queue, &pkt, 1)) {
        av_interleaved_write_frame(s, &pkt);
        av_free_packet(&pkt);
    }

    return NULL;
}


int main(int argc, char *argv[])
{
    IDeckLinkIterator            *deckLinkIterator = CreateDeckLinkIteratorInstance();
    DeckLinkCaptureDelegate     *delegate;
    BMDDisplayMode                selectedDisplayMode = bmdModeNTSC;
    int                            displayModeCount = 0;
    int                            exitStatus = 1;
    int                            connection = 0, camera = 0, i=0;
    int                            ch;
    HRESULT                        result;

    pthread_mutex_init(&sleepMutex, NULL);
    pthread_cond_init(&sleepCond, NULL);
    av_register_all();

    if (!deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }
    // Parse command line options
    while ((ch = getopt(argc, argv, "?hc:s:f:a:m:n:F:C:I:")) != -1)
    {
        switch (ch)
        {
            case 'm':
                g_videoModeIndex = atoi(optarg);
                break;
            case 'c':
                g_audioChannels = atoi(optarg);
                if (g_audioChannels != 2 &&
                    g_audioChannels != 8 &&
                    g_audioChannels != 16)
                {
                    fprintf(stderr, "Invalid argument: Audio Channels must be either 2, 8 or 16\n");
                    goto bail;
                }
                break;
            case 's':
                g_audioSampleDepth = atoi(optarg);
                if (g_audioSampleDepth != 16 && g_audioSampleDepth != 32)
                {
                    fprintf(stderr, "Invalid argument: Audio Sample Depth must be either 16 bits or 32 bits\n");
                    goto bail;
                }
                break;
            case 'f':
                g_videoOutputFile = optarg;
                break;
            case 'n':
                g_maxFrames = atoi(optarg);
                break;
            case 'F':
                fmt = av_guess_format(optarg, NULL, NULL);
		break;
            case 'I':
                connection = atoi(optarg);
		break;
	    case 'C':
		camera = atoi(optarg);
		break;
            case '?':
            case 'h':
                usage(0);
        }
    }

    /* Connect to the first DeckLink instance */
    do {
    	result = deckLinkIterator->Next(&deckLink);
    } while(i++<camera);

    if (result != S_OK)
    {
        fprintf(stderr, "No DeckLink PCI cards found.\n");
        goto bail;
    }

    if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput) != S_OK)
        goto bail;

    result = deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfiguration);
    if (result != S_OK)
    {
        fprintf(stderr, "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n", result);
        goto bail;
    }
    //XXX make it generic
    if (connection == 1) { // video compuesto + audio analogico
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                  bmdVideoConnectionComposite);
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigAudioInputConnection,
				  bmdAudioConnectionAnalog);
    }else if (connection == 2) { // video componentes + audio analogico
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                  bmdVideoConnectionComponent);
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                  bmdAudioConnectionAnalog);
    }else if (connection == 3) { // HDMI video + audio
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                  bmdVideoConnectionHDMI);
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                  bmdAudioConnectionEmbedded);
    }else if (connection == 4) { // SDI video + audio
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoInputConnection,
                                  bmdVideoConnectionSDI);
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigAudioInputConnection,
                                  bmdAudioConnectionEmbedded);
    }
    delegate = new DeckLinkCaptureDelegate();
    deckLinkInput->SetCallback(delegate);

    // Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
    result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
    {
        fprintf(stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result);
        goto bail;
    }

    if (!fmt)
    fmt = av_guess_format(NULL, g_videoOutputFile, NULL);


    if (g_videoModeIndex < 0)
    {
        fprintf(stderr, "No video mode specified\n");
        usage(0);
    }


    selectedDisplayMode = -1;
    while (displayModeIterator->Next(&displayMode) == S_OK)
    {
        if (g_videoModeIndex == displayModeCount)
        {
            selectedDisplayMode = displayMode->GetDisplayMode();
            break;
        }
        displayModeCount++;
        displayMode->Release();
    }

    oc = avformat_alloc_context();
    oc->oformat = fmt;

    snprintf(oc->filename, sizeof(oc->filename), "%s", g_videoOutputFile);

    fmt->video_codec = CODEC_ID_RAWVIDEO;
    fmt->audio_codec = CODEC_ID_PCM_S16LE;

    video_st = add_video_stream(oc, fmt->video_codec);
    audio_st = add_audio_stream(oc, fmt->audio_codec);

    av_set_parameters(oc, NULL);

    if (!(fmt->flags & AVFMT_NOFILE)) {
        if (url_fopen(&oc->pb, oc->filename, URL_WRONLY) < 0) {
            fprintf(stderr, "Could not open '%s'\n", oc->filename);
            exit(1);
        }
    }

    if (selectedDisplayMode < 0)
    {
        fprintf(stderr, "Invalid mode %d specified\n", g_videoModeIndex);
        goto bail;
    }

    result = deckLinkInput->EnableVideoInput(selectedDisplayMode, bmdFormat8BitYUV, 0);
    if(result != S_OK)
    {
        fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
        goto bail;
    }

    result = deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, g_audioSampleDepth, g_audioChannels);
    if(result != S_OK)
    {
        goto bail;
    }
    av_write_header(oc);

    result = deckLinkInput->StartStreams();
    if(result != S_OK)
    {
        goto bail;
    }
    // All Okay.
    exitStatus = 0;

    avpacket_queue_init(&queue);
    pthread_t th;

    if (pthread_create(&th, NULL, push_packet, oc))
        goto bail;

    // Block main thread until signal occurs
    pthread_mutex_lock(&sleepMutex);
    pthread_cond_wait(&sleepCond, &sleepMutex);
    pthread_mutex_unlock(&sleepMutex);
    fprintf(stderr, "Stopping Capture\n");

bail:
    if (displayModeIterator != NULL)
    {
        displayModeIterator->Release();
        displayModeIterator = NULL;
    }

    if (deckLinkInput != NULL)
    {
        deckLinkInput->Release();
        deckLinkInput = NULL;
    }

    if (deckLink != NULL)
    {
        deckLink->Release();
        deckLink = NULL;
    }

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    if (oc != NULL)
    {
        av_write_trailer(oc);
        if (!(fmt->flags & AVFMT_NOFILE)) {
            /* close the output file */
            url_fclose(oc->pb);
        }

    }

    return exitStatus;
}

