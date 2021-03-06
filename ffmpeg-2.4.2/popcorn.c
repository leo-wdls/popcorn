#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/log.h"
#include "libavutil/intmath.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
#include "list.h"


#include <SDL.h>
#include <SDL_thread.h>

#define TAG "[ffplay]  "

#define SDL_VERSION_LIBSDL1P2_DEV
//#define SDL_VERSION_LIBSDL2P0_DEV

//#define __DEBUG_QUEUE__


#define _4K_    (4*1024)

#if 1
#define BMP_PATH  "/data/test_xiaoxin"
#else
#define BMP_PATH  "/home/leo/test"
#endif

#define MAX_VIDEO_PACKET_QUEUE_LEN          (40)
#define MAX_VIDEO_FRAME_QUEUE_LEN            (40)
#define MAX_AUDIO_PACKET_QUEUE_LEN          (10)
#define MAX_QUEUE_SIZE                                  (15*1024*1024)

#define MY_CREATE_OVERLAY_EVENT                (SDL_USEREVENT+1)

static void av_write_log(const char *fmt, ...);

#define __DEBUG_LOG__ 1
#if defined(__DEBUG_LOG__)
#define av_info_log(fmt, args...) printf(TAG fmt, ##args)
//#define av_info_log(fmt, args...) av_write_log(TAG fmt, ##args)
#else
#define av_info_log(fmt, args...)
#endif

#define av_err_log(fmt, args...)  printf(TAG fmt, ##args)
//#define av_err_log(fmt, args...)  av_write_log(TAG fmt, ##args)

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

enum BMP_FORMAT {
   BMP_FMT_RGB24,
   BMP_FMT_RGB32,
   BMP_FMT_MAX,
};

struct image {
   unsigned int width;
   unsigned int height;
   unsigned int format;
};

typedef struct InputParams {
    int  argc;                          //arg number
    char *cmd_name; 
    char *input_file;               //input file path
    char *argv[5];
}InputParams;

typedef enum SteamIndex {
    VIDEO_STREAM_INDEX = 0,
    AUDIO_STREAM_INDEX,
    SUBTILE_STREAM_INDEX,
    MAX_STREAM_INDEX,
} streamIndex;

/*
 * @SHOW_MODE_ONLY_A: only include auido stream
 * @SHOW_MODE_ONLY_V: only include video stream
 * @SHOW_MODE_AV: include video and audio
 */
typedef enum ShowMode {
    SHOW_MODE_NONE = 0X00,
    SHOW_MODE_ONLY_A = 0X01,
    SHOW_MODE_ONLY_V = 0X02, 
    SHOW_MODE_AV = 0X04,
    SHOW_MODE_NB = 0X01 |0X02 |0X04,
}ShowMode;

typedef enum SyncMode {
    SYNC_AUDIO_MASTER = 0x01,   /* default sync mode */
    SYNC_VIDEO_MASTER = 0x02,
    SYNC_EXTER_CLOCK  = 0x03,
    SYNC_MODE_MAX     = 0X03,
}SyncMode;

enum ClockType {
    AUDIO_CLOCK    = 0x01,   /* default sync mode */
    VIDEO_CLOCK    = 0x02,
    EXTER_CLOCK    = 0x03,
    MAX_CLOCK_TYPE = 0X03,
};

typedef struct Clock {
    double   pts;                 /* current packet pts base clock base */              
    //AVRational   last_pts;            /* last packet pts base clock base */
    double   last_update;         /* last update clock */
    double   pts_drift;           /* clock base minus time at which we updated the clock */
                                      /* pts_drift is value which pts relative offset to current time */
    double   speed;               

    SDL_mutex    *mutex;
    int      pause;
    int      pkt_serial;          /* curent packet serial */
    int      *queue_serial;       /* point to packet queue serial */
}Clock;


typedef struct PacketQueueStruct {
    AVPacket                    packet;
    unsigned int                serial;
    int                             is_use;
    struct list_head          packet_list;
} PktQueueStruct;

typedef struct PacketQueueHead {
    char name[32];
    struct list_head   *head;
    int                      max_len;
    int                      nb_packet;
    unsigned int         size;
    unsigned int         serial;
    
    SDL_cond            *cond;
    SDL_mutex          *mutex;
    
} PktQueueHead;

typedef struct FrameQueueHead {
   char name[32];
   struct list_head   *head;
   unsigned int       serial;
   int                max_len;
   int                nb_frame;
   
   SDL_cond          *cond;
   SDL_mutex        *mutex;  
} FrameQueueHead;


typedef struct FrameQueueStruct {
   AVFrame             *frame;
   unsigned int           serial;
   int                       is_use;
   struct list_head   frame_list;
} FrameQueueStruct;

typedef struct HwAudioPara {

    enum AVSampleFormat  format;
    uint64_t                       channel_layout;
    int                               freq;
    int                               channels;
    int                               byte_per_sec;
    int                               frame_size;

}HwAudioPara;

typedef struct Rect {
    int x;
    int y;
    int width;
    int height;
}Rect;

typedef struct Signal {
    SDL_mutex    *mutex;
    SDL_cond     *cond;
    int          condition;
}Signal;

typedef struct VideoState {
    //video&audio decode data
    AVFormatContext                *p_av_formatCtx;
    AVFrame                            *p_av_frame[MAX_STREAM_INDEX];
    AVCodec                            *p_av_codec[MAX_STREAM_INDEX];
    AVCodecContext                 *p_av_codecCtx[MAX_STREAM_INDEX];
    int                                    streams_index[MAX_STREAM_INDEX];

    //packet&frame queue data
    PktQueueHead                    pktQueue[MAX_STREAM_INDEX];
    PktQueueStruct                   vPkt_queue_struct[MAX_VIDEO_PACKET_QUEUE_LEN+1];  
    PktQueueStruct                   aPkt_queue_struct[MAX_AUDIO_PACKET_QUEUE_LEN+1];
    FrameQueueHead                frameQueue[VIDEO_STREAM_INDEX+1];
    FrameQueueStruct               vFrame_queue_struct[MAX_VIDEO_FRAME_QUEUE_LEN+1];

    //audio play data
    HwAudioPara                     hw_audio_para;
    struct SwrContext               *p_a_swrCtx;
    unsigned char                   *audio_buffer;
    int                             audio_buffer_size;

    //display data
    Rect                            screen_size;
    Rect                            cur_win_info;
    Rect                            disp_area;

    //sdl data
#ifdef SDL_VERSION_LIBSDL1P2_DEV
    SDL_Surface                     *surface_bottom, *surface_top;
    SDL_Overlay                     *layer1;
    SDL_Thread                      *read_pkt_tid, *decode_video_tid;
    Signal                          display_signal;

#endif
    int                             paused, force_refresh, stop;

    //sync data
    ShowMode                       show_mode;    
    SyncMode                       sync_mode;
    //AVRational                       last_frame_time;
    //AVRational                     frame_timer;
    Clock                          clock[MAX_CLOCK_TYPE];

    AVRational                     v_frame_rate[2];/*[0] last, [1] current*/                  
}VideoState;


InputParams g_input_param;
VideoState g_video_state;



LIST_HEAD(video_frame_list);
LIST_HEAD(video_packet_list);
LIST_HEAD(audio_packet_list);

static int dump_frame(AVCodecContext *p_v_codecCtx, AVFrame *frame, int dump_count);

/////////////////////////////////////////////////////////////////////////
// make build ok!
const char program_name[] = "ffplay_demon";
const int program_birth_year = 2014;

void show_help_default(const char *opt, const char *arg)
{
   return;
}
/////////////////////////////////////////////////////////////////////////

static int save_bmp_file(char *path, void *data, struct image bmp)
{

   #define BF_TYPE_INDEX                       0X00
   #define BF_SIZE_INDEX                        0X02
   #define BF_RESERVER1_INDEX             0X06
   #define BF_RESERVER2_INDEX             0X08
   #define BF_OFFBITS_INDEX                  0X0A
   #define BI_SIZE_INDEX                        0X0E
   #define BI_WIDTH_INDEX                    0X12
   #define BI_HEIGHT_INDEX                   0X16
   #define BI_PLANES_INDEX                   0X1A
   #define BI_BITCOUNT_INDEX              0X1C
   #define BI_COMPRESSION_INDEX        0X1E
   #define BI_SIZE_IMAGE_INDEX            0X22
   #define BI_XPPM_INDEX                      0X26  //x pels per meter
   #define BI_YPPM_INDEX                      0X2A  //y pels per meter
   #define BI_CLR_USED_INDEX               0X2E
   #define BI_CLR_IMPORTANT                0X32
   
   #define BMP_FILE_HEAD_SIZE        (14)
   #define BMP_INFO_HEAD_SIZE       (40)
 
   #define BMP_HEAD_SIZE                (BMP_FILE_HEAD_SIZE+BMP_INFO_HEAD_SIZE)

   
   unsigned long int count = 0; 
   char bmp_head[BMP_HEAD_SIZE];
   int ret, y;
   FILE *fp = NULL;

   if (path == NULL || data == NULL) {
      av_err_log("parameter abort!\n");
      return -1;
   }


   // RGB24 bmp file

   memset(bmp_head, 0, BMP_HEAD_SIZE);

   memcpy(bmp_head+BF_TYPE_INDEX, "BM", 2);
  
   bmp_head[BF_SIZE_INDEX]   =  (strlen(data) + BMP_HEAD_SIZE) & 0xFF;
   bmp_head[BF_SIZE_INDEX+1] = ((strlen(data) + BMP_HEAD_SIZE) & 0xFF00) >> 8;
   bmp_head[BF_SIZE_INDEX+2] = ((strlen(data) + BMP_HEAD_SIZE) & 0xFF0000) >> 16;
   bmp_head[BF_SIZE_INDEX+3] = ((strlen(data) + BMP_HEAD_SIZE) & 0xFF000000) >> 24;

   memset(bmp_head+BF_RESERVER1_INDEX, 0, 4);
   //bmp_head[BF_RESERVER1_INDEX]   = 0x00;
   //bmp_head[BF_RESERVER1_INDEX+1] = 0x00;
   //bmp_head[BF_RESERVER2_INDEX]   = 0x00;
   //bmp_head[BF_RESERVER1_INDEX+1] = 0x00;

   bmp_head[BI_SIZE_INDEX]   =  BMP_INFO_HEAD_SIZE & 0xff;
   bmp_head[BI_SIZE_INDEX+1] = (BMP_INFO_HEAD_SIZE & 0xff00) >> 8;
   bmp_head[BI_SIZE_INDEX+2] = (BMP_INFO_HEAD_SIZE & 0xff0000) >> 16;
   bmp_head[BI_SIZE_INDEX+3] = (BMP_INFO_HEAD_SIZE & 0xff000000) >> 24;


   bmp_head[BI_WIDTH_INDEX]   =  bmp.width & 0xff;
   bmp_head[BI_WIDTH_INDEX+1] = (bmp.width & 0xff00) >> 8;
   bmp_head[BI_WIDTH_INDEX+2] = (bmp.width & 0xff0000) >> 16;
   bmp_head[BI_WIDTH_INDEX+3] = (bmp.width & 0xff000000) >> 24;

   bmp_head[BI_HEIGHT_INDEX]   =  bmp.height & 0xff;
   bmp_head[BI_HEIGHT_INDEX+1] = (bmp.height & 0xff00) >> 8;
   bmp_head[BI_HEIGHT_INDEX+2] = (bmp.height & 0xff0000) >> 16;
   bmp_head[BI_HEIGHT_INDEX+3] = (bmp.height & 0xff000000) >> 24;

   bmp_head[BI_PLANES_INDEX]   = 0x01;
   bmp_head[BI_PLANES_INDEX+1] = 0x00;

   if (bmp.format == BMP_FMT_RGB24) {
      bmp_head[BI_BITCOUNT_INDEX]   = 24;
      bmp_head[BI_BITCOUNT_INDEX+1] = 0X00;
   }

   memset(bmp_head+BI_COMPRESSION_INDEX, 0, 4);
   //bmp_head[BI_COMPRESSION_INDEX]   = 0; //no compression 
   //bmp_head[BI_COMPRESSION_INDEX+1] = 0;
   //bmp_head[BI_COMPRESSION_INDEX+2] = 0;
   //bmp_head[BI_COMPRESSION_INDEX+3] = 0;

   // when picture compression is BI_RGB, can set 0
   memset(bmp_head+BI_SIZE_IMAGE_INDEX, 0 ,4); 

   fp = fopen(path, "wb");
   if (!fp) {
      av_err_log("open %s fail!\n", path);
      return -1;
   }

   ret = fwrite(bmp_head, BMP_HEAD_SIZE, 1, fp);

   for (y=0; y<bmp.height; y++) {
     ret = fwrite(data + y * bmp.width * (bmp_head[BI_BITCOUNT_INDEX]>>3), 1, bmp.width*(bmp_head[BI_BITCOUNT_INDEX]>>3), fp);
   }

   fclose(fp);

   return 0;   
}

static int save_frame_to_bmp(char *path, void *data, int width, int height, enum AVPixelFormat format)
{

   struct image bmp;

   bmp.width = width;
   bmp.height = height;

   if (format == AV_PIX_FMT_RGB24)
      bmp.format = BMP_FMT_RGB24;

   save_bmp_file(path, data, bmp);

   return 0;
}

static int parseFrame_from_packet(AVCodecParserContext *p_av_codecParserCtx, const AVCodecContext *p_av_codecCtx, 
                           unsigned char *p_srcData, unsigned int srcData_size, AVPacket *p_av_dstPkt)
{
   //only support video
   unsigned char *p_out_buf = NULL;
   unsigned int  out_size = 0;
   int  length = 0;


   length = av_parser_parse2(p_av_codecParserCtx, p_av_codecCtx,
                             &p_out_buf, &out_size,
                             p_srcData, srcData_size,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);

   if (p_out_buf && (out_size != 0)) {
      av_info_log("av_paraser_parse2 seperate a frame as a packet: size=%dK(%dByte)\n", out_size/1024, out_size);
      av_info_log("p_out_buf=0x%x, out_size=%d, length=%d\n", (int)p_out_buf, out_size, length);
      //av_info_log("p_out_buf[0]=0x%x, [1]=0x%x, [2]=0x%x, [3]=0x%x, [4]=0x%x\n", *p_out_buf, *(p_out_buf+1), *(p_out_buf+2), *(p_out_buf+3), *(p_out_buf+4));

   } else {
     av_err_log("av_parser_parse2: No data output to new packet!\n");

   }
          
   av_init_packet(p_av_dstPkt);
   p_av_dstPkt->data = p_out_buf;
   p_av_dstPkt->size = out_size;
   //p_in_buf += length;
   //in_size -= length;

   return length;

}

static int videoFrame_decode(AVCodecContext *p_av_codecCtx, AVFrame *p_av_frame, int *got_frame, const AVPacket *p_av_packet)
{
   *got_frame = 0;
   
   if (p_av_codecCtx->codec->type == AVMEDIA_TYPE_VIDEO) {

       if (avcodec_decode_video2(p_av_codecCtx, p_av_frame, got_frame, p_av_packet) < 0) {
          av_err_log("video decode process fail!\n");
          return -1;

       } 
       return *got_frame;
   } 

   return 0;

}


int scale_image(unsigned char *src_data, int src_lineSize[], 
                int src_W, int src_H, enum AVPixelFormat src_format,
                unsigned char *dst_data[], int dst_lineSize[],
                int dst_W, int dst_H, enum AVPixelFormat dst_format,
                int sws_flag)
{
    struct SwsContext *p_swsCtx = NULL;

    if (!src_data || !dst_data)
       return 0;


    p_swsCtx = sws_getCachedContext(p_swsCtx,
                                   src_W, src_H, src_format,
                                   dst_W, dst_H, dst_format,
                                   sws_flag, NULL, NULL, NULL);

    sws_scale(p_swsCtx, (const unsigned char* const *)src_data, (const int *)src_lineSize, 0, 
                      src_H, dst_data, dst_lineSize);    
    //av_info_log("frame format cover and scale!\n");
    //av_info_log("frame format cover and scale:\n");
    //av_info_log("src_W=%d, src_H=%d, src_format=%d, src_lineSize=%d, dst_W=%d, dst_H=%d, dst_format=%d, src_lineSize=%d\n", 
    //             src_W, src_H, src_format, src_lineSize, dst_W, dst_H, dst_format, dst_lineSize);

    sws_freeContext(p_swsCtx);

    return 0;
}


static void init_player(VideoState *video_states)
{
    //video_states->paused = 0;
    //video_states->stop = 0;
    video_states->show_mode = SHOW_MODE_NONE;
    
    video_states->cur_win_info.x = -1;
    video_states->cur_win_info.y = -1;
    video_states->cur_win_info.width = -1;
    video_states->cur_win_info.height = -1;
}

static void get_inputParam_list(InputParams *param, int argc, char *argv[])
{
    param->argc = argc;
    param->cmd_name = argv[0];
    param->input_file = argv[1];
    return;
}

void signal_init(Signal *signal, int condtion)
{
    signal->cond = SDL_CreateCond();
    signal->mutex = SDL_CreateMutex();
    signal->condition = condtion;
}

void signal_destory(Signal *signal)
{
    SDL_DestroyMutex(signal->mutex);
    SDL_DestroyCond(signal->cond);
}

static SyncMode get_master_syncType(VideoState *video_states)
{
    return video_states->sync_mode;
}

static void set_master_syncType(SyncMode wanted_sync_mode, ShowMode show_mode, SyncMode *actual_sync_mode)
{
    if (wanted_sync_mode == SYNC_VIDEO_MASTER) {
       if ((SHOW_MODE_AV|SHOW_MODE_ONLY_V)&show_mode)
          *actual_sync_mode = SYNC_VIDEO_MASTER;
       else
          *actual_sync_mode = SYNC_AUDIO_MASTER;
       
    } else if (wanted_sync_mode == SYNC_AUDIO_MASTER) {
       if ((SHOW_MODE_AV|SHOW_MODE_ONLY_A)&show_mode)
          *actual_sync_mode = SYNC_AUDIO_MASTER;
       else
          *actual_sync_mode = SYNC_EXTER_CLOCK;
    }
}


/*
 * get_clock: get reference clock
 */
static double get_clock(Clock *clk)
{
    double temp;
    //if (*clk->queue_serial != clk->serial)
    //   return NAN;
    if (clk->pause) {
       SDL_LockMutex(clk->mutex); 
       temp = clk->pts;
       SDL_UnlockMutex(clk->mutex); 
       return temp;

    } else {
       SDL_LockMutex(clk->mutex); 
       temp = clk->last_update+clk->pts_drift;
       SDL_UnlockMutex(clk->mutex); 
       
       return temp;
    }
    return 0;
}

static void set_clock_at(Clock *clk, double pts, double cur_time, int serial)
{
    SDL_LockMutex(clk->mutex); 
    clk->pts = pts;
    clk->last_update= cur_time;
    clk->pkt_serial = serial;
    clk->pts_drift = clk->pts-cur_time;
    SDL_UnlockMutex(clk->mutex); 
}

/*
 * set_clock: set reference clock
 * @pts: reference packet's pts
 * @serial: reference packet's serial
 */
static void set_clock(Clock *clk, double pts, int serial)
{
    //time base ms
    double time;
    time = av_gettime_relative()/1000000.0;
    set_clock_at(clk, pts, time, serial);
}

/*
 * init_clock: init reference clock
 * @queue_serial: packet queue serial
 */
static void init_clock(Clock *clk, int *queue_serial)
{
    clk->pause = 0;
    clk->speed = 1.0;
    clk->pts_drift = 0;
    clk->queue_serial = *queue_serial;
    clk->mutex = SDL_CreateMutex();
    set_clock(clk, 0, -1);
}

static void videoClock_set(VideoState *video_states, FrameQueueStruct *p_fqueue_struct)
{
    int index;
    AVStream  *p_v_stream;

    index = video_states->streams_index[VIDEO_STREAM_INDEX];
    p_v_stream = video_states->p_av_formatCtx->streams[index];
    
    /*record video pts*/
    set_clock(&video_states->clock[VIDEO_CLOCK], 
              p_fqueue_struct->frame->pkt_pts*av_q2d(p_v_stream->time_base), 
              p_fqueue_struct->serial);

}

static void audioClock_set(VideoState *video_states, PktQueueStruct *P_pktqueue_struct)
{
    int index;
    AVStream  *p_a_stream;

    index = video_states->streams_index[AUDIO_STREAM_INDEX];
    p_a_stream = video_states->p_av_formatCtx->streams[index];

    /*record video pts*/
    set_clock(&video_states->clock[AUDIO_CLOCK], 
              P_pktqueue_struct->packet.pts*av_q2d(p_a_stream->time_base),
              P_pktqueue_struct->serial);

}

/*
 * get_master_clock: get master_clock
 */
static double get_master_clock(Clock clk[], SyncMode sync_mode)
{
    double val;

    switch (sync_mode) {
        case SYNC_AUDIO_MASTER:
           val = get_clock(&clk[AUDIO_CLOCK]);
           break;
        case SYNC_VIDEO_MASTER:
           val = get_clock(&clk[VIDEO_CLOCK]);
           break;
        case SYNC_EXTER_CLOCK:
           val = get_clock(&clk[EXTER_CLOCK]);
           break;
        default:
           av_info_log("%s:it's not the %d mode!\n", __FUNCTION__, sync_mode);
           break;
    }

    return val;

}


/*no AV sync correction is done if below the minimum AV sync threshold*/
#define AV_SYNC_THRESHOLD_MAX 0.1
/*AV sync correction is done if above the maximum AV sync threshold*/
#define AV_SYNC_THRESHOLD_MIN 0.04
/*If a frame duration is longer than this, it will not be duplicated to compensate AV sync*/
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/*no AV correction is done if too big error*/
#define AV_NOSYNC_THRESHOLD 10.0

static double correct_videoFrame_delay(VideoState * video_states, Clock clk[], double delay)
{
    double diff, sync_threshold;
    SyncMode sync_mode;

    if (sync_mode == SYNC_VIDEO_MASTER) {
       av_err_log("%s: sync_mode=SYNC_VIDEO_MASTER, error!\n");
       return -1;
    }

    /*if video is slave, we try to correct big delays by duplicate or deleting a frame*/
    sync_mode = get_master_syncType(video_states);
    diff = get_clock(&clk[VIDEO_CLOCK])-get_master_clock(clk, sync_mode);
    /*AV_SYNC_THRESHOLD_MIN<=delay && delay<=AV_SYNC_THRESHOLD_MAX*/
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));

    /*video clk slow than master clk*/
    if (diff <= -sync_threshold) {
       delay = FFMAX(0, delay+diff);
       //av_info_log("video clk slow than master clk! delay=%lf\n", delay);
    }   
    /*video clk fast than master clk*/
    else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
       delay = delay+diff;
       //av_info_log("video clk faster than master clk! and > AV_SYNC_FRAMEDUP_THRESHOLD delay=%lf\n", delay);
    }   
    else if (diff>=sync_threshold) {
       delay = 2* delay;
       //av_info_log("video clk faster than master clk! delay=%lf\n", delay);
    }
    return delay;
}

static void sync_clock_to_slave(Clock *master, Clock *slave)
{

}


//AVFrame *p_av_frame_copy;

static void frameStruct_init( FrameQueueStruct frame_queue_struct[], int len)
{
    int i;

    for (i=0; i< len; i++) {
        memset(&frame_queue_struct[i], 0, sizeof(FrameQueueStruct));
    }
}

static int frameQueueStruct_create(FrameQueueHead *queue_head,
                                   FrameQueueStruct **dst_fQueueStruct, 
                                   FrameQueueStruct src_fQueueStruct[],
                                   AVFrame *src_frame)
{

    int i;

    FrameQueueStruct *p_dst_fQueueStruct;

    SDL_LockMutex(queue_head->mutex);
    //chose a FrameQueueStruct struct which is not used.
    for (i=0; i<queue_head->max_len+1; i++) {

       if (src_fQueueStruct[i].is_use == 0 && src_fQueueStruct[i].frame != NULL) {
          *dst_fQueueStruct = &src_fQueueStruct[i];
          break;
       }
    }

    SDL_UnlockMutex(queue_head->mutex);
    
    if (i >= queue_head->max_len+1) {
       av_err_log("no malloc frameQueueStruct!index =%d\n", i);
       return i;
    }

    
    p_dst_fQueueStruct = *dst_fQueueStruct;
    //copy frame from src to dst
    scale_image(src_frame->data, src_frame->linesize, 
                         src_frame->width, src_frame->height, src_frame->format, 
                         p_dst_fQueueStruct->frame->data, p_dst_fQueueStruct->frame->linesize,
                         src_frame->width, src_frame->height, AV_PIX_FMT_YUV420P,
                         SWS_BICUBIC);

    p_dst_fQueueStruct->frame->width = src_frame->width;
    p_dst_fQueueStruct->frame->height = src_frame->height;
    p_dst_fQueueStruct->frame->format = AV_PIX_FMT_YUV420P;
    p_dst_fQueueStruct->frame->pkt_dts = src_frame->pkt_dts;
    p_dst_fQueueStruct->frame->pkt_pts = src_frame->pkt_pts;
    p_dst_fQueueStruct->frame->pkt_duration = src_frame->pkt_duration;
    //dump_YUV420P_frame(src_frame, 20);
    (*dst_fQueueStruct)->is_use = 1;
    
    return i;
}


static void frameQueueStruct_destroy(FrameQueueHead *queue_head, FrameQueueStruct *fQueueStruct)
{
    int i;

    //destroy a FrameQueueStruct
    SDL_LockMutex(queue_head->mutex);
    queue_head->nb_frame--;
    fQueueStruct->is_use = 0;
    fQueueStruct->frame_list.prev = NULL;
    fQueueStruct->frame_list.next = NULL;
#ifdef __DEBUG_QUEUE__
    av_info_log("%s: %s queue current len is %d\n", __FUNCTION__, queue_head->name, queue_head->nb_frame);
#endif
    SDL_UnlockMutex(queue_head->mutex);

}
static void frameQueue_init(FrameQueueHead *queue_head, struct list_head *head, int max_len, char *name)
{
    memset(queue_head, 0, sizeof(FrameQueueHead));

    if (name && strlen(name) + 1 <= 32)
       memcpy(queue_head->name, name, (strlen(name)+1));
    else
       av_err_log("frameQueue name too long or NULL!len=%d\n", strlen(name)+1);

    queue_head->nb_frame = 0;
    queue_head->max_len = max_len;
    queue_head->head = head;
    queue_head->serial = 0;
    queue_head->cond = SDL_CreateCond();
    queue_head->mutex = SDL_CreateMutex();
    
}


static void frameQueue_destroy(FrameQueueHead *queue_head)
{
    queue_head->head = NULL; 
    SDL_DestroyCond(queue_head->cond);
    SDL_DestroyMutex(queue_head->mutex);
}


static int frameQueue_queue(FrameQueueHead *queue_head, FrameQueueStruct *frameQueue_struct)
{
    //Insert a FrameQueueStruct to frame queue
    if (queue_head->nb_frame < queue_head->max_len) {
       SDL_LockMutex(queue_head->mutex);
       list_add(&(frameQueue_struct->frame_list), queue_head->head->prev);
       queue_head->nb_frame++;
       frameQueue_struct->serial = queue_head->serial;
       SDL_UnlockMutex(queue_head->mutex);
 #ifdef __DEBUG_QUEUE__
       av_info_log("%s: %s queue current len is %d\n", __FUNCTION__, queue_head->name, queue_head->nb_frame);
 #endif
       return 0;

    } else {
       av_err_log("%s: queue len is full, len=%d\n", __FUNCTION__, queue_head->nb_frame);
       return -1;

    }
}



static int frameQueue_dequeue(FrameQueueHead *queue_head, FrameQueueStruct **frameQueue_struct)
{
    struct list_head *list;

    //delete a FrameQueueStruct to frame queue
    if (queue_head->nb_frame > 0) {
       SDL_LockMutex(queue_head->mutex); 
       list = queue_head->head->next;
       list_remove(list, queue_head->head);
       //*frameQueue_struct = container_of(list, struct FrameQueueStruct, frame_list);
       *frameQueue_struct = ({ const struct list_head  *__mptr = (list); (struct FrameQueueStruct *)( (char *)__mptr - ((size_t)&((struct FrameQueueStruct *)0)->frame_list) );});
       SDL_UnlockMutex(queue_head->mutex); 
       return 0;
    } else {
       av_err_log("%s: queue len is empty!\n", __FUNCTION__);
       return -1;
    }
}

static int frameQueue_is_full(FrameQueueHead *queue_head)
{
    int ret;
    SDL_LockMutex(queue_head->mutex);
    ret = queue_head->nb_frame < queue_head->max_len ? 0: 1;
    SDL_UnlockMutex(queue_head->mutex);

    return ret;
}

static int frameQueue_is_empty(FrameQueueHead *queue_head)
{
    int ret;
    SDL_LockMutex(queue_head->mutex);
    ret = queue_head->nb_frame == 0 ? 1: 0;
    SDL_UnlockMutex(queue_head->mutex);

    return ret;
}

static void videoPicture_get(FrameQueueHead *p_v_fqueue_head, FrameQueueStruct **p_fqueue_struct)
{

    /* get frame from frameQueue */
    while (frameQueue_is_empty(p_v_fqueue_head)) {
        SDL_LockMutex(p_v_fqueue_head->mutex);
        SDL_CondWait(p_v_fqueue_head->cond, p_v_fqueue_head->mutex);
        SDL_UnlockMutex(p_v_fqueue_head->mutex);
    }
    frameQueue_dequeue(p_v_fqueue_head, p_fqueue_struct);
}

static void videPicture_destory(FrameQueueHead *p_v_fqueue_head, FrameQueueStruct *p_fqueue_struct)
{
    frameQueueStruct_destroy(p_v_fqueue_head, p_fqueue_struct);
    SDL_LockMutex(p_v_fqueue_head->mutex);
    SDL_CondSignal(p_v_fqueue_head->cond);
    SDL_UnlockMutex(p_v_fqueue_head->mutex);

}

static int pktQueueStruct_create(PktQueueHead *queue_head,
                                                        PktQueueStruct **dst_pktQueueStruct,
                                                        PktQueueStruct src_pktQueueStruct[], 
                                                        AVPacket *packet)
{
    int i;

    SDL_LockMutex(queue_head->mutex);

    //chose a PktQueueStruct struct which is not used.
    for (i=0; i<queue_head->max_len+1; i++) {

       if (src_pktQueueStruct[i].is_use == 0) {
          *dst_pktQueueStruct = &src_pktQueueStruct[i];
          break;
       }
    }

    if (i >=queue_head->max_len+1) {
       av_err_log("no malloc packetQueueStruct!index=%d\n", i);
       SDL_UnlockMutex(queue_head->mutex);   
       return i;
    }

    memset(*dst_pktQueueStruct, 0, sizeof(PktQueueStruct));

    //copy packet
    // if (av_dup_packet(&(*dst_pktQueueStruct)->packet) < 0) {
    if (av_copy_packet(&(*dst_pktQueueStruct)->packet, packet) < 0) {
       av_free_packet(&(*dst_pktQueueStruct)->packet);

       (*dst_pktQueueStruct)->is_use = 0;
       SDL_UnlockMutex(queue_head->mutex);    
       return -1;
    }

    (*dst_pktQueueStruct)->is_use = 1;
    SDL_UnlockMutex(queue_head->mutex);   

    return i;
}



static int pktQueueStruct_destroy(PktQueueHead *queue_head, PktQueueStruct *pkt_queue_struct)
{

    SDL_LockMutex(queue_head->mutex);
    av_free_packet(&pkt_queue_struct->packet);
    av_init_packet(&pkt_queue_struct->packet);
    pkt_queue_struct->packet_list.prev = NULL;
    pkt_queue_struct->packet_list.next = NULL;
    pkt_queue_struct->serial = 0;
    pkt_queue_struct->is_use = 0;
    queue_head->nb_packet--;
    SDL_UnlockMutex(queue_head->mutex);    
#ifdef __DEBUG_QUEUE__
    av_info_log("%s: %s queue current len is %d\n", __FUNCTION__, queue_head->name, queue_head->nb_packet);
#endif
    
    return 0;
}



static  int inline packetQueue_get_len(PktQueueHead *queue_head)
{
    return queue_head->nb_packet;
    
}



static int packetQueue_is_full(PktQueueHead *queue_head)
{

    int ret;
    SDL_LockMutex(queue_head->mutex); 
    ret = queue_head->nb_packet < queue_head->max_len? 0: 1;
    SDL_UnlockMutex(queue_head->mutex); 
    
    return ret;
}



static int packetQueue_is_empty(PktQueueHead *queue_head)
{
    int ret;
    SDL_LockMutex(queue_head->mutex); 
    ret = queue_head->nb_packet ==0? 1: 0;
    SDL_UnlockMutex(queue_head->mutex); 
    
    return ret;
}



static int packetQueue_flush(PktQueueHead *queue_head)
{
   PktQueueStruct *packet_queue_struct;

#if 0   
   SDL_LockMutex(queue_head->mutex);

   //list_for_each_entry(packet_queue_struct, queue_head->head, packet_list) {
   for (packet_queue_struct=({ const struct list_head *__mptr = ((queue_head->head)->next); (strcut list_head *)( (char *)__mptr - ((size_t)&((strcut list_head *)0)->packet_list) );}); &packet_queue_struct->packet_list!=(queue_head->head); packet_queue_struct=({ const typeof( ((typeof(*packet_queue_struct) *)0)->packet_list ) *__mptr = (packet_queue_struct->packet_list.next); (typeof(*packet_queue_struct) *)( (char *)__mptr - ((size_t)&((struct list_head *)0)->packet_list) );})) {

      av_free_packet(&packet_queue_struct->packet);
      packet_queue_struct->serial = 0;
      packet_queue_struct->packet_list = NULL;
   }

   
   queue_head->head = NULL;
   queue_head->size = 0;
   queue_head->nb_packet = 0;
   av_info_log("%s!\n", __FUNCTION__);
    
   SDL_UnlockMutex(queue_head->mutex);
#endif  
   return 0;
}

static void packetStruct_init( PktQueueStruct  pkt_queue_struct[], int len)
{
    int i;

    for (i=0; i< len; i++) {
        memset(&pkt_queue_struct[i], 0, sizeof(PktQueueStruct));
    }
}

static int packetQueue_init(PktQueueHead *queue_head, struct list_head *head, int max_len, char *name)
{
    memset(queue_head, 0 , sizeof(PktQueueHead));

    if (name && strlen(name) + 1 <= 32)
       memcpy(queue_head->name, name, (strlen(name)+1));
    else
       av_err_log("packetQueue name too long or NULL!len=%d\n", strlen(name)+1);
    
    queue_head->size = 0;
    queue_head->nb_packet = 0;
    queue_head->max_len = max_len;
    queue_head->serial = 0;
    queue_head->head = head;
    //queue_head->list_last = head;
    
    queue_head->cond = SDL_CreateCond();
    queue_head->mutex = SDL_CreateMutex();
    
    return 0;
}



static int packetQueue_destroy(PktQueueHead *queue_head)
{   
    if (queue_head->nb_packet) {
       packetQueue_flush(queue_head);
    }   

    queue_head->head = NULL;  
    SDL_DestroyCond(queue_head->cond);
    SDL_DestroyMutex(queue_head->mutex);
    
    return 0;
}


static int packetQueue_queue(PktQueueHead *queue_head, PktQueueStruct *pkt_queue_struct)
{
   int ret = 0;

   if (!queue_head && !pkt_queue_struct) {
      av_err_log("%s: parameter err!\n");   
      return -1;
   }
   
   SDL_LockMutex(queue_head->mutex); 
   //Insert a PktQueueStruct to packet queue
   if (queue_head->nb_packet < queue_head->max_len) {
      list_add(&pkt_queue_struct->packet_list, queue_head->head->prev);
      queue_head->nb_packet++;  
      queue_head->size += pkt_queue_struct->packet.size;
      pkt_queue_struct->serial = ++queue_head->serial;
#ifdef __DEBUG_QUEUE__
      av_info_log("%s: %s queue current len is %d\n",__FUNCTION__, queue_head->name, queue_head->nb_packet);
#endif     
      ret = 0;  
   } else {
      av_err_log("%s, queue is full! max_len=%d, len=%d\n", __FUNCTION__, queue_head->max_len, queue_head->nb_packet);   
      ret = -1;
   }
   
   SDL_UnlockMutex(queue_head->mutex);
   
   return ret;
}

static int packetQueue_dequeue(PktQueueHead *queue_head, PktQueueStruct **pkt_queue_struct)
{
   int ret;
   struct list_head *list;

   //delete a PktQueueStruct to packet queue
   //PktQueueStruct *packet_queue_struct;
   SDL_LockMutex(queue_head->mutex);   
   if (queue_head->nb_packet > 0) {
      list = queue_head->head->next;
      list_remove(list, queue_head->head);
      //*pkt_queue_struct = list_entry(list, PacketQueueStruct, packet_list);
      //*pkt_queue_struct = ({ const typeof( ((struct PacketQueueStruct *)0)->packet_list )  *__mptr = (list); (struct PacketQueueStruct *)( (char *)__mptr - ((size_t)&((struct PacketQueueStruct *)0)->packet_list) );});
      *pkt_queue_struct = ({ const struct list_head  *__mptr = (list); (struct PacketQueueStruct *)( (char *)__mptr - ((size_t)&((struct PacketQueueStruct *)0)->packet_list) );});     
      queue_head->size -= (*pkt_queue_struct)->packet.size;
      ret = 0;
   } else {
      av_err_log("%s, queue is empty! len=%d\n", __FUNCTION__, queue_head->nb_packet);   
      ret = -1;  
   }
   SDL_UnlockMutex(queue_head->mutex);
   
   return ret;
}

static int audioFrame_decode(VideoState *video_states) 
{
    AVFrame        *p_a_frame;
    AVCodecContext *p_a_codecCtx;
    AVStream       *p_a_stream;
    struct SwrContext  *p_a_swrCtx;

    PktQueueHead *p_a_pktQueueHead;
    PktQueueStruct *p_pktqueue_struct;

    HwAudioPara   *p_a_hwPara;

    int got_frame = -1;
    int ret;
    int data_size;
    int resampled_data_size;
    int serial;
    int index;
 
    p_a_frame = video_states->p_av_frame[AUDIO_STREAM_INDEX];
    p_a_codecCtx = video_states->p_av_codecCtx[AUDIO_STREAM_INDEX];
    p_a_pktQueueHead = &video_states->pktQueue[AUDIO_STREAM_INDEX];
    p_a_hwPara = &video_states->hw_audio_para;
    p_a_swrCtx = video_states->p_a_swrCtx;
    index = video_states->streams_index[AUDIO_STREAM_INDEX];
    p_a_stream = video_states->p_av_formatCtx->streams[index];

    
    while (1) {
    
       if (p_a_frame) 
          av_frame_unref(p_a_frame);

       while (packetQueue_is_empty(p_a_pktQueueHead)) {
           SDL_LockMutex(p_a_pktQueueHead->mutex);
           SDL_CondWait(p_a_pktQueueHead->cond, p_a_pktQueueHead->mutex);
           SDL_UnlockMutex(p_a_pktQueueHead->mutex);
        }

        if (packetQueue_dequeue(p_a_pktQueueHead, &p_pktqueue_struct) >= 0) {
            
        if (p_a_codecCtx->codec->type == AVMEDIA_TYPE_AUDIO) {
               ret = avcodec_decode_audio4(p_a_codecCtx, p_a_frame, &got_frame, &p_pktqueue_struct->packet);
               if (ret < 0) {
                 av_err_log("avcodec decode audio frame fail!\n");
                 return -1;
           }
               serial = p_pktqueue_struct->serial;
               pktQueueStruct_destroy(p_a_pktQueueHead, p_pktqueue_struct);
               SDL_LockMutex(p_a_pktQueueHead->mutex);
               SDL_CondSignal(p_a_pktQueueHead->cond);
               SDL_UnlockMutex(p_a_pktQueueHead->mutex);
        }
        
        }

        if (!got_frame) {
           av_err_log("audio decode a frame fail\!\n");
           continue;
        } else {
           //av_info_log("audio decode a frame success!\n");
        }
     
        /*record audio pts*/
        audioClock_set(video_states, p_pktqueue_struct);

        data_size = av_samples_get_buffer_size(NULL, 
                                                   av_frame_get_channels(p_a_frame), 
                                                   p_a_frame->nb_samples, 
                                                   p_a_frame->format, 
                                                   1);

         if (p_a_hwPara->format != p_a_frame->format ||
             p_a_hwPara->freq != p_a_frame->sample_rate ||
             p_a_hwPara->channel_layout != p_a_frame->channel_layout ||
             p_a_hwPara->channels != p_a_frame->channels ) {

             if (p_a_swrCtx) 
                swr_free(&p_a_swrCtx);
             
             p_a_swrCtx = swr_alloc_set_opts(NULL, 
                                                  p_a_hwPara->channel_layout, p_a_hwPara->format, p_a_hwPara->freq,
                                                  p_a_frame->channel_layout, p_a_frame->format, p_a_frame->sample_rate,
                                                             0, NULL);
             if (!p_a_swrCtx ||swr_init(p_a_swrCtx) < 0) {
                av_err_log(
                           "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                            p_a_frame->sample_rate, av_get_sample_fmt_name(p_a_frame->format), av_frame_get_channels(p_a_frame),
                            p_a_hwPara->freq, av_get_sample_fmt_name(p_a_hwPara->format), p_a_hwPara->channels);
                break;
             }


         }     

       if (p_a_swrCtx) {
             unsigned char **in_data = p_a_frame->extended_data;
             unsigned char **out_data =&video_states->audio_buffer;
          int out_nb_samples = p_a_frame->nb_samples+256;
             int out_size = av_samples_get_buffer_size(NULL, p_a_hwPara->channels, out_nb_samples,  p_a_hwPara->format, 0);
             int len;
             
             if (out_size < 0) {
                av_err_log("av_samples_get_buffer_size() fail!\n");
                break;
             }
             
             av_fast_malloc(&video_states->audio_buffer, &video_states->audio_buffer_size, out_size);
             if (!video_states->audio_buffer) {
                av_err_log("av_fast_malloc() alloc fail!\n");
                return AVERROR(ENOMEM);
             }
             
             // swr_convert return samples count
          len = swr_convert(p_a_swrCtx, out_data, out_nb_samples, in_data, p_a_frame->nb_samples);

             if (len < 0) {
                av_err_log("swr_convert() fail!\n");
                break;
             }
             
             if (len == out_nb_samples) {
                av_err_log("audio buffer is probably too small!\n");
                swr_init(p_a_swrCtx);
             }

             resampled_data_size = len * av_get_bytes_per_sample(p_a_hwPara->format) * p_a_hwPara->channels ;
             
          }


     //video_states->audio_buffer = p_a_frame->data[0];

        return resampled_data_size;


   }
   av_info_log("audioFrame_decode exit!\n");
   return 0;
}


static void audio_callback(void* userdata, unsigned char *stream, int len)
{
   int len1;
   static int audio_buf_index = 0, audio_buf_size = 0;
   VideoState *video_states;
   Clock *audio_clk;
   HwAudioPara  *p_a_hwPara;
   double time;

   video_states = (VideoState *)userdata;
   audio_clk = &video_states->clock[AUDIO_CLOCK];
   p_a_hwPara = &video_states->hw_audio_para;
   //av_info_log("call %s!\n", __FUNCTION__);
   
   while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
           audio_buf_size = audioFrame_decode(video_states);
         if (audio_buf_size < 0) {
                av_err_log("audioFrame_decode get buffer size is zero!\n");
              
         } else {
                audio_buf_index = 0;
             
             }  
         }
         len1 = audio_buf_size - audio_buf_index;

         len1 = (len1 > len) ? len: len1;

         memcpy(stream, (unsigned char *)video_states->audio_buffer + audio_buf_index, len1);

         stream += len1;
         len -= len1;
         audio_buf_index += len1;
      
    }
 
    /*if a frame audio date > len, need correct audio pts*/
    //av_info_log("correct time before:%lf\n", av_q2d(audio_clk->pts));
    time = audio_clk->pts-(audio_buf_size-audio_buf_index)/p_a_hwPara->byte_per_sec;
    set_clock(audio_clk, time, audio_clk->pkt_serial);
    //av_info_log("correct time after:%lf\n", av_q2d(audio_clk->pts));
    
}

static void dump_audioPara(VideoState *video_states)
{
    HwAudioPara   *p_a_hwPara;
    p_a_hwPara = &video_states->hw_audio_para;
    
    av_err_log("dump audio hw parameter:   \n");
    printf ("          audio channels: %d\n",               p_a_hwPara->channels);
    printf ("          audio channels layout: 0x%x\n",   p_a_hwPara->channel_layout);
    printf ("          audio format: %d\n",                   p_a_hwPara->format);
    printf ("          audio freq: %dhz\n",                   p_a_hwPara->freq);
    printf ("          audio frame size: %d bytes\n",     p_a_hwPara->frame_size);
    printf ("          audio byte_per_sec: %d bytes\n", p_a_hwPara->byte_per_sec);

}


static int audio_open(VideoState *video_states, 
                                  int64_t wanted_channel_layout, 
                                  int wanted_channel_nb, 
                                  int sample_rate, 
                                  HwAudioPara *hw_audio_para)
{
    SDL_AudioSpec wanted_spec, actual_spec;
    const char *env;
    
    env = SDL_getenv("SDL_AUDIO_CHANNELS");

    //av_info_log("call %s!\n", __FUNCTION__);

    if (env) {
       wanted_channel_nb = atoi(env);
       wanted_channel_layout = av_get_default_channel_layout(wanted_channel_nb);
    }

    if (!wanted_channel_layout ||wanted_channel_nb != av_get_channel_layout_nb_channels(wanted_channel_layout))  {
        //why do it?
        wanted_channel_layout = av_get_default_channel_layout(wanted_channel_nb);
    wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    wanted_channel_nb = av_get_channel_layout_nb_channels(wanted_channel_layout);

    wanted_spec.channels = wanted_channel_nb;
    wanted_spec.freq = sample_rate;
    //signedl 16-bit samples in native byte order
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2<<ff_log2(wanted_spec.freq/SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = video_states;

    if (SDL_OpenAudio(&wanted_spec, &actual_spec) < 0) {
       av_err_log("SDL_OpenAudio fail!\n");
       return -1;
    }

    if (actual_spec.channels != wanted_spec.channels) {
       wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
       if (!wanted_channel_layout) {
          av_err_log("sdl channel count %d not support.\n", actual_spec.channels);
          return -1;
       }
    }
    
    if (actual_spec.format != AUDIO_S16SYS) {
       av_err_log("Could not support 'AUDIO_S16SYS' format!\n");
       return -1;
    }

    hw_audio_para->channels = actual_spec.channels;
    hw_audio_para->channel_layout = wanted_channel_layout;
    hw_audio_para->format = AV_SAMPLE_FMT_S16;
    hw_audio_para->freq = actual_spec.freq;
    hw_audio_para->frame_size = av_samples_get_buffer_size(NULL, hw_audio_para->channels, 
                                                               1, hw_audio_para->format, 1);
    hw_audio_para->byte_per_sec = av_samples_get_buffer_size(NULL, hw_audio_para->channels, 
                                                                hw_audio_para->freq, hw_audio_para->format, 1);

    //dump_audioPara(video_states);

    if (hw_audio_para->frame_size <= 0) {
        av_err_log("av_samples_get_buffer_size failed\n");
        return -1;
    }

    return actual_spec.size;
    
}
//the thread read packet from input file
static int readPacket_thread(void *ptr)
{
    AVPacket                  pkt;
    AVFormatContext       *p_av_formatCtx;
    AVCodecContext        *p_v_codecCtx;
    PktQueueStruct         *p_pktqueue_struct;
    PktQueueHead          *p_v_pktQueueHead, *p_a_pktQueueHead;
    VideoState               *video_states;

    int ret;

    
    video_states = (VideoState *)ptr;
    p_av_formatCtx = video_states->p_av_formatCtx;
    //p_v_codecCtx = video_states->p_av_codecCtx[VIDEO_STREAM_INDEX];
    p_v_pktQueueHead = &video_states->pktQueue[VIDEO_STREAM_INDEX];
    p_a_pktQueueHead = &video_states->pktQueue[AUDIO_STREAM_INDEX];


    while (1) {
       av_init_packet(&pkt);
       
       if (av_read_frame(p_av_formatCtx, &pkt) < 0) {
          av_err_log("av_read_frame fail!\n");
          break;
       }

       p_pktqueue_struct = NULL;
       
       if (pkt.stream_index == video_states->streams_index[VIDEO_STREAM_INDEX]) {
          ret = pktQueueStruct_create(p_v_pktQueueHead, 
                                                           &p_pktqueue_struct,
                                                           video_states->vPkt_queue_struct,  
                                                           &pkt);
        
          while (packetQueue_is_full(p_v_pktQueueHead)) { 
               SDL_LockMutex(p_v_pktQueueHead->mutex);      
               SDL_CondWait(p_v_pktQueueHead->cond, p_v_pktQueueHead->mutex);
               SDL_UnlockMutex(p_v_pktQueueHead->mutex);
          }    
          packetQueue_queue(p_v_pktQueueHead, p_pktqueue_struct);
          SDL_LockMutex(p_v_pktQueueHead->mutex); 
          SDL_CondSignal(p_v_pktQueueHead->cond);
          SDL_UnlockMutex(p_v_pktQueueHead->mutex);

       }
       else if (pkt.stream_index == video_states->streams_index[AUDIO_STREAM_INDEX]) {
            ret = pktQueueStruct_create(p_a_pktQueueHead, 
                                                           &p_pktqueue_struct,
                                                           video_states->aPkt_queue_struct,  
                                                           &pkt);
            while (packetQueue_is_full(p_a_pktQueueHead)) { 
                 SDL_LockMutex(p_a_pktQueueHead->mutex);      
                 SDL_CondWait(p_a_pktQueueHead->cond, p_a_pktQueueHead->mutex);
                 SDL_UnlockMutex(p_a_pktQueueHead->mutex);
            }    
            packetQueue_queue(p_a_pktQueueHead, p_pktqueue_struct);
            SDL_LockMutex(p_a_pktQueueHead->mutex); 
            SDL_CondSignal(p_a_pktQueueHead->cond);
            SDL_UnlockMutex(p_a_pktQueueHead->mutex);


       }

     
       else if (pkt.stream_index == video_states->streams_index[SUBTILE_STREAM_INDEX]) {

       }

       av_free_packet(&pkt);
     
    }
    av_info_log("readPacket thread exit!\n");
    return 0;
}


//the thread decode video packet
static int decodeVideo_thread(void *ptr)
{

    //AVFormatContext       *p_av_formatCtx;
    AVFrame                   *p_v_frame;
    AVCodecContext        *p_v_codecCtx;
    PktQueueStruct         *pkt_queue_struct;
    PktQueueHead          *p_v_pktQueueHead;
    FrameQueueHead      *p_v_frameQueueHead;
    FrameQueueStruct     *fQueueStruct;
    VideoState               *video_states;


    int got_frame = 0;
    int ret, i;
    int byte_count;
    unsigned char *buffer[MAX_VIDEO_FRAME_QUEUE_LEN+1];

    video_states = (VideoState *)ptr;
    p_v_frame = video_states->p_av_frame[VIDEO_STREAM_INDEX];
    p_v_codecCtx = video_states->p_av_codecCtx[VIDEO_STREAM_INDEX];
    p_v_pktQueueHead = &video_states->pktQueue[VIDEO_STREAM_INDEX];
    p_v_frameQueueHead = &video_states->frameQueue[VIDEO_STREAM_INDEX];
    

    for (i=0; i<MAX_VIDEO_FRAME_QUEUE_LEN+1; i++) {
        fQueueStruct = &video_states->vFrame_queue_struct[i];
        fQueueStruct->frame = NULL;
        buffer[i] = NULL;
     
        fQueueStruct->frame =  av_frame_alloc();
        if (!fQueueStruct->frame) {
            av_err_log("av_frame_alloc fail at %d: index=%d\n", __LINE__, i);
            fQueueStruct->frame = NULL;
            continue;
        }

        // caculate picture size
        byte_count = avpicture_get_size(AV_PIX_FMT_YUV420P, p_v_codecCtx->width, p_v_codecCtx->height);

        // alloc a size memrry of picture
        buffer[i] = (unsigned char *)av_malloc(byte_count * sizeof(unsigned char));
        if (!buffer[i]) {
           av_err_log("av_malloc buffer fail at %d, buffer_size=%d, index=%d\n", __LINE__, (byte_count * sizeof(unsigned char)), i);
           continue;        
        }  
    }

    for (i=0; i<MAX_VIDEO_FRAME_QUEUE_LEN+1; i++) {
        fQueueStruct = &video_states->vFrame_queue_struct[i];
        if (fQueueStruct->frame && buffer[i]) {
           avpicture_fill((AVPicture *)fQueueStruct->frame, buffer[i], AV_PIX_FMT_YUV420P,  p_v_codecCtx->width, p_v_codecCtx->height);
           
        } else {
           if (!buffer[i])  
               av_free(buffer[i]);
           if (!fQueueStruct->frame) 
               av_frame_free(&fQueueStruct->frame);
        
        }
    }


    while(1) {
        
        while (packetQueue_is_empty(p_v_pktQueueHead)) {
            SDL_LockMutex(p_v_pktQueueHead->mutex);
            SDL_CondWait(p_v_pktQueueHead->cond, p_v_pktQueueHead->mutex);
            SDL_UnlockMutex(p_v_pktQueueHead->mutex);
        }

        if (packetQueue_dequeue(p_v_pktQueueHead, &pkt_queue_struct) >= 0) {
           videoFrame_decode(p_v_codecCtx, p_v_frame, &got_frame, (const AVPacket *)&pkt_queue_struct->packet);
           
           pktQueueStruct_destroy(p_v_pktQueueHead, pkt_queue_struct);
           SDL_LockMutex(p_v_pktQueueHead->mutex);
           SDL_CondSignal(p_v_pktQueueHead->cond);           
           SDL_UnlockMutex(p_v_pktQueueHead->mutex);
           /*if frame decode fail, serial also add count*/
           p_v_frameQueueHead->serial++;
           if (got_frame) {
              //av_info_log("video decode a frame success!\n");

              ret = frameQueueStruct_create(p_v_frameQueueHead,
                                            &fQueueStruct, 
                                            video_states->vFrame_queue_struct, 
                                            p_v_frame);
              
              while (frameQueue_is_full(video_states->frameQueue) ) {
                  SDL_LockMutex(p_v_frameQueueHead->mutex);
                  SDL_CondWait(p_v_frameQueueHead->cond, p_v_frameQueueHead->mutex);
                  SDL_UnlockMutex(p_v_frameQueueHead->mutex); 
              }

              frameQueue_queue(p_v_frameQueueHead, fQueueStruct);
              SDL_LockMutex(p_v_frameQueueHead->mutex);
              SDL_CondSignal(p_v_frameQueueHead->cond);
              SDL_UnlockMutex(p_v_frameQueueHead->mutex); 

           } else {
              av_err_log("video decode a frame fail!\n");
              continue;
           }

        }
        
    }

    return 0;

}


static int stream_open(VideoState *video_states, char *input_file)
{
    int i;
    int err;
    
    video_states->streams_index[VIDEO_STREAM_INDEX] = -1;
    video_states->streams_index[AUDIO_STREAM_INDEX] = -1;
    video_states->streams_index[SUBTILE_STREAM_INDEX] = -1;
    
    // register all format of video file, and codec
    av_info_log("av register!\n");
    av_register_all();

    // open video file, get head info from file
    av_info_log("open input media file!\n");
    err = avformat_open_input(&video_states->p_av_formatCtx, input_file, NULL, NULL);
    if (err < 0) {
       av_err_log("avformat_open_input open %s fail, err=%d\n", input_file, err);
       return -1;
    }

    // get stream info(AVFormatContext)
    av_info_log("get stream info!\n");
    err = avformat_find_stream_info(video_states->p_av_formatCtx, NULL);
    if (err<0) {
       av_err_log("can not find codec parameter!\n");
       avformat_close_input(&video_states->p_av_formatCtx);
       return -1;
    }

    // Dump information about file onto standard error
    av_dump_format(video_states->p_av_formatCtx, 0, input_file, 0);

    av_info_log("nb_streams = %d\n", video_states->p_av_formatCtx->nb_streams);

    // get video/audio/subTitle stream
    for (i=0; i<video_states->p_av_formatCtx->nb_streams; i++) {
       if (video_states->p_av_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
          video_states->streams_index[VIDEO_STREAM_INDEX] = i;
          //pts log
          //av_info_log("video stream duration =%d\n", 
          //               video_states->p_av_formatCtx->streams[i]->duration);
       }
       else if (video_states->p_av_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
          video_states->streams_index[AUDIO_STREAM_INDEX] = i;
          //pts log
          //av_info_log("audio stream duration =%d\n", 
          //               video_states->p_av_formatCtx->streams[i]->duration);
           
       }
       else if (video_states->p_av_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_SUBTITLE) {
          video_states->streams_index[SUBTILE_STREAM_INDEX] = i;
           
       }
    }

   return 0;
}

static int stream_close(VideoState *video_states)
{
     avformat_close_input(&video_states->p_av_formatCtx);

     return 0;
}

static void caculate_displayRect(Rect *area,
                                                     int win_w, int win_h, 
                                                     int pict_w, int pict_h, 
                                                     AVRational aspect_ratio) 
{
    int width, height;
    float ratio;
    if (aspect_ratio.num == 0) 
     ratio =0.0;
    else
        ratio = av_q2d(aspect_ratio);

    if (ratio <= 0.0) {
       ratio = 1.0;
       //caculate aspect ratio
       ratio *= (float)pict_w/(float)pict_h;
    }

    height = win_h;
    width = ((int)rint(height*ratio)) & ~1;
    height = ((int)rint(width/ratio)) & ~1;

    if (width > win_w) {
       width = win_w;
       height = ((int)rint(width/ratio)) & ~1;  
       width = ((int)rint(height*ratio)) & ~1;  
    }

    area->x = (win_w - width)/2;
    area->y = (win_h - height)/2;    
    area->width = FFMAX(width, 1);
    area->height = FFMAX(height, 1);


    //av_info_log("display area:w=%d, h=%d, x=%d, y=%d\n", area->width, area->height, area->x, area->y);
    
}


static void set_default_winSize(const AVCodecContext *p_av_codecCtx, Rect *p_dispArea, Rect *p_cur_winInfo)
{

    AVRational aspect_ratio;
    
    aspect_ratio.num = 0;
    

    p_cur_winInfo->width = p_av_codecCtx->width;
    p_cur_winInfo->height = p_av_codecCtx->height;

    av_info_log("window size:w=%d, h=%d\n", p_cur_winInfo->width, p_cur_winInfo->height);   
    caculate_displayRect(p_dispArea, 
                       p_cur_winInfo->width, p_cur_winInfo->height, 
                       p_av_codecCtx->width, p_av_codecCtx->height,
                       aspect_ratio);

}

static int videoMode_open(VideoState *video_states, Rect *p_dispArea, Rect *p_cur_winInfo)
{
    AVRational aspect_ratio;
    int flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL | SDL_RESIZABLE;

    aspect_ratio.num = 0;
    
    
    video_states->surface_top = SDL_SetVideoMode(p_cur_winInfo->width, p_cur_winInfo->height, 0, flags);
    if (!video_states->surface_top) {
       av_err_log("SDL SetVideoMode fail!\n");
       return -1;           
    }   

    video_states->layer1 = SDL_CreateYUVOverlay(p_dispArea->width, p_dispArea->height, SDL_YV12_OVERLAY, video_states->surface_top);
    if (!video_states->layer1) {
       av_err_log("SDL CreateYUVOverlay fail!\n");
       return -1;           
    }

    return 0;
}

static int stream_component_open(VideoState *video_states, enum AVMediaType stream_type)
{
    AVCodecContext *p_av_codecCtx;
    AVCodec      *p_av_codec;
    enum AVMediaType codec_type;
    enum AVCodecID     codec_id;

    int index, index_type;
    int ret;

    switch (stream_type) {
        case AVMEDIA_TYPE_VIDEO:
             index_type = VIDEO_STREAM_INDEX;
             break;
              
        case AVMEDIA_TYPE_AUDIO:
             index_type = AUDIO_STREAM_INDEX;
             break;
              
        case AVMEDIA_TYPE_SUBTITLE:
             index_type = SUBTILE_STREAM_INDEX;
             break;
        default:
             index_type = AVMEDIA_TYPE_UNKNOWN;
             break;
   } 

   index = video_states->streams_index[index_type];

   if (!video_states) 
      return -1;

   if (video_states->streams_index[index]== -1 || stream_type>= AVMEDIA_TYPE_NB) {
      av_err_log("stream_index=%d, stream_type/max=%d/%d\n", 
                       video_states->streams_index[index], stream_type, AVMEDIA_TYPE_NB);
      return -1;
   }

    p_av_codecCtx = video_states->p_av_formatCtx->streams[index]->codec;
    codec_id = video_states->p_av_formatCtx->streams[index]->codec->codec_id;

    codec_type = video_states->p_av_formatCtx->streams[index]->codec->codec_type;
    
    p_av_codec = avcodec_find_decoder(codec_id);
    if (!p_av_codec) {
       av_err_log("codec not support! codec_id=%d, codec_name=%s, codec_type=%d\n", 
                   codec_id, avcodec_get_name(codec_id), codec_type);
       return -1;     
    }

    memcpy(&p_av_codecCtx->codec_name[0], avcodec_get_name(codec_id), 32);

    if (codec_type != stream_type) {
       av_err_log("stream_index  not match stream_type! index=%d, stream_type=%d\n", index, stream_type);
       return -1;
    }

     // open codec
     ret = avcodec_open2(p_av_codecCtx, p_av_codec,  NULL); 
     if (ret < 0) {
        av_err_log("open %s(id=%d) codec fail!\n", avcodec_get_name(codec_id), codec_id);
        return -1;
     }   
            
    switch (codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            video_states->p_av_codecCtx[VIDEO_STREAM_INDEX] = p_av_codecCtx;
            video_states->p_av_codec[VIDEO_STREAM_INDEX] = p_av_codec;

            av_info_log("find video decoder sucess! name=\"%s\",id=%d\n", avcodec_get_name(codec_id), codec_id);

            packetQueue_init(&video_states->pktQueue[VIDEO_STREAM_INDEX], 
                                         &video_packet_list, 
                                         MAX_VIDEO_PACKET_QUEUE_LEN,
                                         "video_pkt");
            packetStruct_init(video_states->vPkt_queue_struct, MAX_VIDEO_PACKET_QUEUE_LEN+1);

           
            frameQueue_init(&video_states->frameQueue[VIDEO_STREAM_INDEX], 
                                        &video_frame_list,
                                        MAX_VIDEO_FRAME_QUEUE_LEN,
                                        "vidoe_frame");
            frameStruct_init(video_states->vFrame_queue_struct, MAX_VIDEO_FRAME_QUEUE_LEN+1);

            
                
            video_states->p_av_frame[VIDEO_STREAM_INDEX] = av_frame_alloc();
            if (!video_states->p_av_frame[VIDEO_STREAM_INDEX]) {
               av_err_log("alloc frame fail at %d\n", __LINE__);
               return -1;
            }

            /*get frame rate*/
            video_states->v_frame_rate[0] =
            video_states->v_frame_rate[1] = av_guess_frame_rate(video_states->p_av_formatCtx, 
                                            video_states->p_av_formatCtx->streams[index], 
                                            NULL);

            av_info_log("frame rate is %lf\n", av_q2d(video_states->v_frame_rate[1]));
            /*Init SDL_Mutex*/
            signal_init(&video_states->display_signal, 1);


#ifdef SDL_VERSION_LIBSDL1P2_DEV
            video_states->read_pkt_tid = SDL_CreateThread(readPacket_thread, video_states);
            video_states->decode_video_tid = SDL_CreateThread(decodeVideo_thread, video_states);
            set_default_winSize(p_av_codecCtx, &video_states->disp_area, &video_states->cur_win_info);
            if (videoMode_open(video_states, &video_states->disp_area, &video_states->cur_win_info) < 0) 
          return -1;

#elif defined(SDL_VERSION_LIBSDL2P0_DEV)
           SDL_CreateThread(readPacket_thread, "read packet", video_states);
           SDL_CreateThread(decodeVideo_thread, "video_decode", video_states);
                
#endif

           break;
        
     case AVMEDIA_TYPE_AUDIO:

          video_states->p_av_frame[AUDIO_STREAM_INDEX] = av_frame_alloc();
          if (!video_states->p_av_frame[AUDIO_STREAM_INDEX]) {
             av_err_log("alloc frame fail at %d\n", __LINE__);
             return -1;
          }
          
          video_states->p_av_codecCtx[AUDIO_STREAM_INDEX] = p_av_codecCtx;
          video_states->p_av_codec[AUDIO_STREAM_INDEX] = p_av_codec;
          ret = audio_open(video_states, p_av_codecCtx->channel_layout, 
                                 p_av_codecCtx->channels, p_av_codecCtx->sample_rate, 
                                 &video_states->hw_audio_para);
 
          if (ret < 0) {
             av_err_log("audio_open fail!\n");
             return -1;
          }

          packetQueue_init(&video_states->pktQueue[AUDIO_STREAM_INDEX], 
                               &audio_packet_list,
                               MAX_AUDIO_PACKET_QUEUE_LEN,
                               "audio_pkt");
          packetStruct_init(video_states->aPkt_queue_struct, MAX_AUDIO_PACKET_QUEUE_LEN+1);

          SDL_PauseAudio(0);
          break;
        
     case AVMEDIA_TYPE_SUBTITLE:
          video_states->p_av_codecCtx[SUBTILE_STREAM_INDEX] = p_av_codecCtx;
          video_states->p_av_codec[SUBTILE_STREAM_INDEX] = p_av_codec;
          break;
        
     default :
          av_err_log("%s:%d codec_type=%d\n", __FUNCTION__, __LINE__, codec_type);
          break;
    }


    return 0;
}

static int stream_component_close(VideoState *video_states, int stream_index)
{
    return 0;
}

static int dump_YUV420P_frame(AVFrame *frame, int dump_count)
{
    AVFrame *p_v_frame = NULL;
        
    int buffer_size;
    unsigned char *p_buf = NULL;
    char path[256];
    static int count = 0;

    if (count > dump_count)
       return 0;
    
    p_v_frame =  av_frame_alloc();
    if (!p_v_frame) {
       av_err_log("av_frame_alloc at %d.\n", __LINE__);
       return -1;
    }

    buffer_size = avpicture_get_size(AV_PIX_FMT_RGB24, frame->width, frame->height);
    
    p_buf = (unsigned char *)av_malloc(buffer_size * sizeof(unsigned char));
    if (!p_buf) {
       av_err_log("av_malloc buffer fail at %d, buffer_size=%ld.\n", __LINE__, (buffer_size * sizeof(unsigned char)));   
       av_frame_free(&p_v_frame);       
       return -1;
    }

    avpicture_fill((AVPicture *)p_v_frame, p_buf, AV_PIX_FMT_RGB24, frame->width, frame->height);
   
    scale_image(frame->data, frame->linesize, 
                       frame->width, frame->height, frame->format, 
                       p_v_frame->data, p_v_frame->linesize, 
                       frame->width, frame->height, AV_PIX_FMT_RGB24,
                       SWS_BICUBIC);

    p_v_frame->width = frame->width;
    p_v_frame->height = frame->height;
    p_v_frame->format = AV_PIX_FMT_RGB24;

    sprintf(path, "%s/%d.bmp", BMP_PATH, count);
    save_frame_to_bmp(path, p_v_frame->data[0], frame->width, frame->height, AV_PIX_FMT_RGB24);

    count++;
    av_free(p_buf);
    av_frame_free(&p_v_frame);  
    
    return 0;
}



static int videoImage_display(SDL_Overlay *layer, Rect *dispArea, AVFrame *frame)
{
    AVPicture pict = {{0}};
    SDL_Rect rect;

    SDL_LockYUVOverlay(layer);
    

    pict.data[0] = layer->pixels[0];
    pict.data[1] = layer->pixels[2];
    pict.data[2] = layer->pixels[1];
    pict.linesize[0] = layer->pitches[0];
    pict.linesize[1] = layer->pitches[2];
    pict.linesize[2] = layer->pitches[1];

    scale_image(frame->data, frame->linesize, 
                         frame->width, frame->height, frame->format, 
                         pict.data, pict.linesize,
                         dispArea->width, dispArea->height, AV_PIX_FMT_YUV420P,
                         SWS_BICUBIC);
    
    rect.x = dispArea->x;
    rect.y = dispArea->y;
    rect.w = dispArea->width;
    rect.h = dispArea->height;
 
    SDL_UnlockYUVOverlay(layer);

    //dump_YUV420P_frame(frame, 20)
    //av_info_log("pitch1=%d, pitch2=%d,pitch3=%d\n",  frame->linesize[0], frame->linesize[1],  frame->linesize[2]);
    SDL_DisplayYUVOverlay(layer, &rect);
    
    return 1;
}


struct timer1_param {
   AVFrame    *p_v_frame;
   VideoState *video_states;
};

static int vFrame_display(int interval, void *param)
{
   VideoState *video_states;
   AVFrame    *p_v_frame;
   Signal     *signal;

   video_states = ((struct timer1_param *)param)->video_states;
   p_v_frame = ((struct timer1_param *)param)->p_v_frame;
   signal = &video_states->display_signal;

   while (signal->condition != 0) {
       SDL_LockMutex(signal->mutex);
       SDL_CondWait(signal->cond, signal->mutex);
       SDL_UnlockMutex(signal->mutex);
   }
   
   videoImage_display(video_states->layer1, &video_states->disp_area, p_v_frame);

   video_states->display_signal.condition = 2;
   SDL_LockMutex(signal->mutex);
   SDL_CondSignal(signal->cond);
   SDL_UnlockMutex(signal->mutex);

   /*if return 0, timer will be cancel*/
   return 0;
}


#if 0
static int videoFrame_refresh(VideoState *video_states, FrameQueueHead *p_v_fqueue_head)
{
    AVFrame              *p_v_frame;
    FrameQueueStruct *frameQueue_struct;

    while (frameQueue_is_empty(p_v_fqueue_head)) {
        SDL_LockMutex(p_v_fqueue_head->mutex);
        SDL_CondWait(p_v_fqueue_head->cond, p_v_fqueue_head->mutex);
        SDL_UnlockMutex(p_v_fqueue_head->mutex);
    }

    frameQueue_dequeue(p_v_fqueue_head, &frameQueue_struct);
    p_v_frame = frameQueue_struct->frame;
    //dump_YUV420P_frame(p_av_frame, 20);
    SDL_Delay(0);
    videoImage_display(video_states->layer1, &video_states->disp_area,  p_v_frame);
    frameQueueStruct_destroy(p_v_fqueue_head, frameQueue_struct);
    SDL_LockMutex(p_v_fqueue_head->mutex);
    SDL_CondSignal(p_v_fqueue_head->cond);
    SDL_UnlockMutex(p_v_fqueue_head->mutex);

    return 0;
    
}
#endif

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

#if 1
static int try_videoFrame_refresh(VideoState *video_states, FrameQueueHead *p_v_fqueue_head, FrameQueueStruct **p_fqueue_struct, double *remaining_time)
{
    AVFormatContext      *p_av_formatCtx;
    AVStream             *p_v_stream;
    double               delay;
    SDL_TimerID          timer_id;
    struct timer1_param  timer_param;
    Signal               *signal;
    Clock                *audio_clk;
    int                  index;
    double               time;

    signal = &video_states->display_signal;
    index = video_states->streams_index[VIDEO_STREAM_INDEX];
    p_v_stream = video_states->p_av_formatCtx->streams[index];
    /* signal condition is 1, when it's init
     * when 1: get frame from frameQueue, 
     * when 0: display video frame to overlay
      * when 2: destroy FrameQueueStruct
     */
    while (signal->condition != 1) {
        SDL_LockMutex(signal->mutex);
        SDL_CondWait(signal->cond, signal->mutex);
        SDL_UnlockMutex(signal->mutex);
    }


    if (*remaining_time == REFRESH_RATE) {
       videoPicture_get(p_v_fqueue_head, p_fqueue_struct);
       videoClock_set(video_states, *p_fqueue_struct);
    } else if (*remaining_time > REFRESH_RATE) {
 
    }

    /* correct video frame delay*/
    delay = av_q2d(av_inv_q(video_states->v_frame_rate[1]));
    delay = correct_videoFrame_delay(video_states, video_states->clock, delay);

    time= av_gettime_relative()/1000000.0; //s
    audio_clk = &video_states->clock[AUDIO_CLOCK];
    if (time < get_clock(audio_clk)+delay) {
       *remaining_time = FFMIN(get_clock(audio_clk)+delay-time, *remaining_time);
       return 0;
    }

    
    //delay = av_add_q(delay, av_inv_q(video_states->v_frame_rate[1]));
    //av_info_log("%s: correct_videoFrame_delay=%lf\n", __FUNCTION__, av_q2d(delay));
    signal->condition = 0;
    SDL_LockMutex(signal->mutex);
    SDL_CondSignal(signal->cond);
    SDL_UnlockMutex(signal->mutex);
    
    timer_param.p_v_frame = (*p_fqueue_struct)->frame;
    timer_param.video_states = video_states;
    timer_id = SDL_AddTimer(delay*1000, vFrame_display, &timer_param);
    //timer_id = SDL_AddTimer(100, vFrame_display, &timer_param);
    //dump_YUV420P_frame(p_av_frame, 20);
    //SDL_Delay(50);
    while (signal->condition != 2) {
        SDL_LockMutex(signal->mutex);
        SDL_CondWait(signal->cond, signal->mutex);
        SDL_UnlockMutex(signal->mutex);
    }
    
    videPicture_destory(p_v_fqueue_head, *p_fqueue_struct);
    signal->condition = 1;
    //SDL_LockMutex(signal->mutex);
    //SDL_CondSignal(signal->cond);
    //SDL_UnlockMutex(signal->mutex);

    return 1;
    
}
#endif


#if 0
static int try_videoFrame_refresh(VideoState *video_states, FrameQueueHead *p_v_fqueue_head)
{
    AVFrame              *p_v_frame;
    AVFormatContext      *p_av_formatCtx;
    AVStream             *p_v_stream;
    FrameQueueStruct     *frameQueue_struct;
    AVRational           delay;
    SDL_TimerID          timer_id;
    struct timer1_param  timer_param;
    Signal               *signal;
    Clock                *audio_clk;
    int                  index;
    int64_t              time;

    signal = &video_states->display_signal;
    index = video_states->streams_index[VIDEO_STREAM_INDEX];
    p_v_stream = video_states->p_av_formatCtx->streams[index];
    /* signal condition is 1, when it's init
     * when 1: get frame from frameQueue, 
     * when 0: display video frame to overlay
      * when 2: destroy FrameQueueStruct
     */
    while (signal->condition != 1) {
        SDL_LockMutex(signal->mutex);
        SDL_CondWait(signal->cond, signal->mutex);
        SDL_UnlockMutex(signal->mutex);
    }

    /* get frame from frameQueue */
    while (frameQueue_is_empty(p_v_fqueue_head)) {
        SDL_LockMutex(p_v_fqueue_head->mutex);
        SDL_CondWait(p_v_fqueue_head->cond, p_v_fqueue_head->mutex);
        SDL_UnlockMutex(p_v_fqueue_head->mutex);
    }

    frameQueue_dequeue(p_v_fqueue_head, &frameQueue_struct);
    p_v_frame = frameQueue_struct->frame;

    /*record video pts*/
    set_clock_at(&video_states->clock[VIDEO_CLOCK], 
               av_mul_q(av_make_q(p_v_frame->pkt_dts, 1), p_v_stream->time_base),
               av_make_q(0, 1), frameQueue_struct->serial);

    delay = correct_videoFrame_delay(video_states, video_states->clock, av_inv_q(video_states->v_frame_rate[1]));

    time = av_gettime_relative(); //ms
    audio_clk = &video_states->clock[AUDIO_CLOCK];
    if (time < av_q2d(get_clock(audio_clk))+av_q2d(delay)) {

    }
    //delay = av_add_q(delay, av_inv_q(video_states->v_frame_rate[1]));
    //av_info_log("%s: correct_videoFrame_delay=%lf\n", __FUNCTION__, av_q2d(delay));
    signal->condition = 0;
    SDL_LockMutex(signal->mutex);
    SDL_CondSignal(signal->cond);
    SDL_UnlockMutex(signal->mutex);
    
    timer_param.p_v_frame = p_v_frame;
    timer_param.video_states = video_states;
    timer_id = SDL_AddTimer(av_q2d(delay)*1000, vFrame_display, &timer_param);
    //timer_id = SDL_AddTimer(100, vFrame_display, &timer_param);
    //dump_YUV420P_frame(p_av_frame, 20);
    //SDL_Delay(50);
    while (signal->condition != 2) {
        SDL_LockMutex(signal->mutex);
        SDL_CondWait(signal->cond, signal->mutex);
        SDL_UnlockMutex(signal->mutex);
    }
    
    frameQueueStruct_destroy(p_v_fqueue_head, frameQueue_struct);
    SDL_LockMutex(p_v_fqueue_head->mutex);
    SDL_CondSignal(p_v_fqueue_head->cond);
    SDL_UnlockMutex(p_v_fqueue_head->mutex);

    signal->condition = 1;
    //SDL_LockMutex(signal->mutex);
    //SDL_CondSignal(signal->cond);
    //SDL_UnlockMutex(signal->mutex);

    return 0;
    
}
#endif

/*
 * check_showMode, choose show mode base stream's type
 */
static ShowMode check_showMode(int streams_index[])
{
    int i;
    ShowMode show_mode = SHOW_MODE_NONE;
    int have_audio = 0, have_video =0;

    
    for (i=0; i<SUBTILE_STREAM_INDEX; i++) {
         if ((streams_index[i] != -1) &&(i == VIDEO_STREAM_INDEX))
            have_video = 1;
         if ((streams_index[i] != -1) &&(i == AUDIO_STREAM_INDEX))
            have_audio = 1;   
    }

    if (have_audio && have_video)
        show_mode = SHOW_MODE_AV;
    else if (have_audio)
        show_mode = SHOW_MODE_ONLY_A;
    else if (have_video)
        show_mode = SHOW_MODE_ONLY_V;
    
    return show_mode;
}


static int refresh_loopWait_event(VideoState *video_states, SDL_Event *event, FrameQueueStruct **p_fqueue_struct)
{
    double remaining_time = 0.0;  //s
    
    SDL_PumpEvents();
    
#if defined(SDL_VERSION_LIBSDL1P2_DEV)
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_ALLEVENTS))
#elif defined(SDL_VERSION_LIBSDL2P0_DEV)
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
#endif
    {
        if (remaining_time > 0.0)
           av_usleep((int64_t)(remaining_time * 1000000.0));
        
        remaining_time = REFRESH_RATE;

        try_videoFrame_refresh(video_states, 
                       &video_states->frameQueue[VIDEO_STREAM_INDEX],
                       p_fqueue_struct, 
                       &remaining_time);

        SDL_PumpEvents();
    }



    return 0;
}


static int event_loop(VideoState *video_states)
{
    SDL_Event event;
    FrameQueueStruct *p_fqueue_struct = NULL;
    AVCodecContext *p_av_codecCtx;
    AVRational aspect_ratio;
    aspect_ratio.num = 0;  //temp

    p_av_codecCtx = video_states->p_av_codecCtx[VIDEO_STREAM_INDEX];

    while(1) {
       refresh_loopWait_event(video_states, &event, &p_fqueue_struct);
       switch (event.type) {
           case SDL_KEYDOWN:
           switch (event.key.keysym.sym) {
               case SDLK_q:
                  exit(1);
                  break;
                
            }
            case SDL_VIDEORESIZE:
                video_states->cur_win_info.width = event.resize.w;
                video_states->cur_win_info.height= event.resize.h;
                caculate_displayRect(&video_states->disp_area,
                         video_states->cur_win_info.width, video_states->cur_win_info.height, 
                         p_av_codecCtx->width, p_av_codecCtx->height,
                         aspect_ratio);
             case MY_CREATE_OVERLAY_EVENT:
          videoMode_open(video_states, &video_states->disp_area, &video_states->cur_win_info);
          break;
       }
    }
}

int main(int argc, char *argv[])
{
    int flag;
    char dummy_videodriver[] = "SDL_VIDEODRIVER=dummy"; 


    init_player(&g_video_state);
    get_inputParam_list(&g_input_param, argc, argv);

    if (stream_open(&g_video_state, g_input_param.input_file) < 0)
       return -1;

    flag = SDL_INIT_EVENTTHREAD | SDL_INIT_VIDEO| SDL_INIT_AUDIO| SDL_INIT_TIMER;
    
    g_video_state.show_mode = check_showMode(g_video_state.streams_index);
    
    //av_info_log("show mode is %x\n", g_video_state.show_mode);
    
    if (g_video_state.show_mode == SHOW_MODE_ONLY_A) {
       flag &= ~SDL_INIT_VIDEO;
       
    } else if (g_video_state.show_mode == SHOW_MODE_AV) {
       set_master_syncType(SYNC_AUDIO_MASTER, g_video_state.show_mode, &g_video_state.sync_mode);
       
       if (g_video_state.sync_mode == SYNC_AUDIO_MASTER) {
           init_clock(&g_video_state.clock[VIDEO_CLOCK], &g_video_state.pktQueue[VIDEO_STREAM_INDEX].serial);
           init_clock(&g_video_state.clock[AUDIO_CLOCK], &g_video_state.pktQueue[AUDIO_STREAM_INDEX].serial);
       }
       
       stream_component_open(&g_video_state, AVMEDIA_TYPE_AUDIO);
       stream_component_open(&g_video_state, AVMEDIA_TYPE_VIDEO);
       
       const SDL_VideoInfo *vi = SDL_GetVideoInfo();
       g_video_state.screen_size.x = -1;
       g_video_state.screen_size.y = -1;
       g_video_state.screen_size.width = vi->current_w;
       g_video_state.screen_size.height = vi->current_h;

    }

    
    if (SDL_Init(flag)) {
       av_err_log("could not initializ SDL-%s\n", SDL_GetError());
       return -1;
    }

     event_loop(&g_video_state);
    // log_sys_exit(&g_video_state);
    
} 
