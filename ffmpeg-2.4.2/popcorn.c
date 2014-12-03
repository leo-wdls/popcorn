#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/log.h"
#include "list.h"


#include <SDL.h>
#include <SDL_thread.h>

#define TAG "[ffplay]  "

#define SDL_VERSION_LIBSDL1P2_DEV
//#define SDL_VERSION_LIBSDL2P0_DEV


#define _4K_    (4*1024)

#if 0
#define BMP_PATH  "/data/test_xiaoxin"
#else
#define BMP_PATH  "/home/leo/test"
#endif

#define MAX_VIDEO_PACKET_QUEUE_LEN         (5)
#define MAX_VIDEO_FRAME_QUEUE_LEN            (5)
#define MAX_QUEUE_SIZE                                  (15*1024*1024)

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
    int  argc;
    char *cmd_name;
    char *input_file;
    char *argv[5];
}InputParams;

typedef enum SteamIndex {
     VIDEO_STREAM_INDEX = 0,
     AUDIO_STREAM_INDEX,
     SUBTILE_STREAM_INDEX,
     MAX_STREAM_INDEX,
} streamIndex;

typedef enum ShowMode {
    SHOW_MODE_NONE = 0X00,
    SHOW_MODE_VIDEO = 0X01,
    SHOW_MODE_AUDIO = 0X02,
    SHOW_MODE_NB = 0X01 |0X02,
}ShowMode;


typedef struct PacketQueueStruct {
    AVPacket                    packet;
    unsigned int                serial;
    int                             is_use;
    struct list_head          packet_list;
} PktQueueStruct;

typedef struct PacketQueueHead {
    struct list_head   *head;// *list_last;
    int                      max_len;
    int                      nb_packet;
    unsigned int         size;
    unsigned int         serial;
    
    SDL_cond            *cond;
    SDL_mutex          *mutex;
    
} PktQueueHead;

typedef struct FrameQueueHead {
   struct list_head   *head;
   int                max_len;
   int                nb_frame;
   
   SDL_cond          *cond;
   SDL_mutex        *mutex;  
} FrameQueueHead;


typedef struct FrameQueueStruct {
   AVFrame             *frame;
   int                       is_use;
   struct list_head   frame_list;
} FrameQueueStruct;



typedef struct VideoState {
    AVFormatContext               *p_av_formatCtx;
    AVFrame                            *p_av_frame[MAX_STREAM_INDEX];
    AVCodec                            *p_av_codec[MAX_STREAM_INDEX];
    AVCodecContext                *p_av_codecCtx[MAX_STREAM_INDEX];
    PktQueueHead                    pktQueue[MAX_STREAM_INDEX];
    PktQueueStruct                   vPkt_queue_struct[MAX_VIDEO_PACKET_QUEUE_LEN+1];  
    FrameQueueHead                frameQueue[VIDEO_STREAM_INDEX+1];
    FrameQueueStruct              vFrame_queue_struct[MAX_VIDEO_FRAME_QUEUE_LEN+1];
    int                                       streams_index[MAX_STREAM_INDEX];
#ifdef SDL_VERSION_LIBSDL1P2_DEV
    SDL_Surface                        *surface_bottom, *surface_top;
    SDL_Overlay                        *layer1;
    SDL_Thread                         *read_pkt_tid, *decode_video_tid;
#endif
    int                                        paused, force_refresh, stop;
    int                                        show_mode;    
}VideoState;


InputParams g_input_param;
VideoState g_video_state;



LIST_HEAD(video_frame_list);
LIST_HEAD(video_packet_list);

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

   #define BF_TYPE_INDEX                  0X00
   #define BF_SIZE_INDEX                  0X02
   #define BF_RESERVER1_INDEX             0X06
   #define BF_RESERVER2_INDEX             0X08
   #define BF_OFFBITS_INDEX               0X0A
   #define BI_SIZE_INDEX                  0X0E
   #define BI_WIDTH_INDEX                 0X12
   #define BI_HEIGHT_INDEX                0X16
   #define BI_PLANES_INDEX                0X1A
   #define BI_BITCOUNT_INDEX              0x1C
   #define BI_COMPRESSION_INDEX           0X1E
   #define BI_SIZE_IMAGE_INDEX            0X22
   #define BI_XPPM_INDEX                  0X26  //x pels per meter
   #define BI_YPPM_INDEX                  0X2A  //y pels per meter
   #define BI_CLR_USED_INDEX              0X2E
   #define BI_CLR_IMPORTANT               0X32
   
   #define BMP_FILE_HEAD_SIZE      (14)
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

static int get_video_frame(AVCodecContext *p_av_codecCtx, AVFrame *p_av_frame, int *got_frame, const AVPacket *p_av_packet)
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
                unsigned char *dst_data, int dst_lineSize[],
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
    video_states->paused = 0;
    video_states->paused = 0;
    video_states->stop = 0;
    video_states->show_mode = SHOW_MODE_NONE;
}

static void get_input_param_list(InputParams *param, int argc, char *argv[])
{
    param->argc = argc;
    param->cmd_name = argv[0];
    param->input_file = argv[1];
    return;
}

//AVFrame *p_av_frame_copy;

static void frameStruct_init(VideoState *video_states)
{
    int i;

    for (i=0; i< MAX_VIDEO_FRAME_QUEUE_LEN+1; i++) {
        memset(&video_states->vFrame_queue_struct[i], 0, sizeof(FrameQueueStruct));
    }
}

static int frameQueueStruct_create(FrameQueueHead *queue_head,
                                   FrameQueueStruct **dst_fQueueStruct, 
                                   FrameQueueStruct src_fQueueStruct[],
                                   AVFrame *src_frame)
{

    int i;

    FrameQueueStruct *p_dst_fQueueStruct;
    AVPicture pict = {{0}};

    SDL_LockMutex(queue_head->mutex);
    
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
    
    pict.data[0] = p_dst_fQueueStruct->frame->data[0];
    pict.data[1] = p_dst_fQueueStruct->frame->data[2];
    pict.data[2] = p_dst_fQueueStruct->frame->data[1];
    pict.linesize[0] = p_dst_fQueueStruct->frame->linesize[0];
    pict.linesize[1] = p_dst_fQueueStruct->frame->linesize[2];
    pict.linesize[2] = p_dst_fQueueStruct->frame->linesize[1];
    
    scale_image(src_frame->data, src_frame->linesize, 
                         src_frame->width, src_frame->height, src_frame->format, 
                         pict.data, pict.linesize,
                         src_frame->width, src_frame->height, AV_PIX_FMT_YUV420P,
                         SWS_BICUBIC);

    p_dst_fQueueStruct->frame->width = src_frame->width;
    p_dst_fQueueStruct->frame->height = src_frame->height;
    p_dst_fQueueStruct->frame->format = AV_PIX_FMT_YUV420P;
    //dump_YUV420P_frame(src_frame, 20);
    (*dst_fQueueStruct)->is_use = 1;
    
    return i;
}


static void frameQueueStruct_destroy(FrameQueueHead *queue_head, FrameQueueStruct *fQueueStruct)
{
    int i;

    SDL_LockMutex(queue_head->mutex);
    queue_head->nb_frame--;
    fQueueStruct->is_use = 0;
    fQueueStruct->frame_list.prev = NULL;
    fQueueStruct->frame_list.next = NULL;

    av_info_log("%s: queue current len is %d\n", __FUNCTION__, queue_head->nb_frame);
    SDL_UnlockMutex(queue_head->mutex);

}
static void frameQueue_init(FrameQueueHead *queue_head, struct list_head *head, int max_len)
{
    memset(queue_head, 0, sizeof(FrameQueueHead));

    queue_head->nb_frame = 0;
    queue_head->max_len = max_len;
    queue_head->head = head;
    queue_head->cond = SDL_CreateCond();
    queue_head->mutex = SDL_CreateMutex();
    
}


static void frameQueue_destroy(FrameQueueHead *queue_head)
{
    queue_head->head = NULL; 
    SDL_DestroyCond(queue_head->cond);
    SDL_DestroyMutex(queue_head->mutex);
}


static int frameQueue_put(FrameQueueHead *queue_head, FrameQueueStruct *frameQueue_struct)
{
    if (queue_head->nb_frame < queue_head->max_len) {
       SDL_LockMutex(queue_head->mutex);
       list_add(&(frameQueue_struct->frame_list), queue_head->head->prev);
       queue_head->nb_frame++;
       SDL_UnlockMutex(queue_head->mutex);
       av_info_log("%s: queue current len is %d\n", __FUNCTION__, queue_head->nb_frame);
       return 0;

    } else {
       av_err_log("%s: queue len is full, len=%d\n", __FUNCTION__, queue_head->nb_frame);
       return -1;

    }
}



static int frameQueue_get(FrameQueueHead *queue_head, FrameQueueStruct **frameQueue_struct)
{
    struct list_head *list;

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

static int pktQueueStruct_create(PktQueueHead *queue_head,
                                                        PktQueueStruct **dst_pktQueueStruct,
                                                        PktQueueStruct src_pktQueueStruct[], 
                                                        AVPacket *packet)
{
    int i;
    
    SDL_LockMutex(queue_head->mutex);

    for (i=0; i<queue_head->max_len+1; i++) {

       if (src_pktQueueStruct[i].is_use == 0) {
          *dst_pktQueueStruct = &src_pktQueueStruct[i];
          break;
       }
    }

    if (i >= queue_head->max_len+1) {
       av_err_log("no malloc packetQueueStruct!index=%d\n", i);
       SDL_UnlockMutex(queue_head->mutex);   
       return i;
    }

    memset(*dst_pktQueueStruct, 0, sizeof(PktQueueStruct));
    
    if (av_copy_packet(&(*dst_pktQueueStruct)->packet, packet) < 0) {
       av_free_packet(&(*dst_pktQueueStruct)->packet);
       av_err_log("%s: duplicate a packet fail!\n", __FUNCTION__);
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
    av_info_log("%s: queue current len is %d\n", __FUNCTION__, queue_head->nb_packet);
    SDL_UnlockMutex(queue_head->mutex);    
    
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

static void packetStruct_init(VideoState *video_states)
{
    int i;

    for (i=0; i< MAX_VIDEO_PACKET_QUEUE_LEN+1; i++) {
        memset(&video_states->vPkt_queue_struct[i], 0, sizeof(PktQueueStruct));
    }
}

static int packetQueue_init(PktQueueHead *queue_head, struct list_head *head, int max_len)
{
    memset(queue_head, 0 , sizeof(PktQueueHead));
    
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


static int packetQueue_put(PktQueueHead *queue_head, PktQueueStruct *pkt_queue_struct)
{
   int ret = 0;

   if (!queue_head && !pkt_queue_struct) {
      av_err_log("%s: parameter err!\n");   
      return -1;
   }
   
   SDL_LockMutex(queue_head->mutex); 
   
   if (queue_head->nb_packet < queue_head->max_len) {
      list_add(&pkt_queue_struct->packet_list, queue_head->head->prev);
      queue_head->nb_packet++;  
      queue_head->size += pkt_queue_struct->packet.size;
      pkt_queue_struct->serial = ++queue_head->serial;
      av_info_log("%s: queue current len is %d\n", __FUNCTION__, queue_head->nb_packet);
      
      ret = 0;  
   } else {
     av_err_log("%s, queue is full! max_len=%d, len=%d\n", __FUNCTION__, queue_head->max_len, queue_head->nb_packet);   
      ret = -1;
   }
   
   SDL_UnlockMutex(queue_head->mutex);
   
   return ret;
}

static int packetQueue_get(PktQueueHead *queue_head, PktQueueStruct **pkt_queue_struct)
{
   int ret;
   struct list_head *list;

   
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


//the thread read packet from input file
static int read_packet_thread(void *ptr)
{
    AVPacket                  pkt;
    AVFormatContext       *p_av_formatCtx;
    AVCodecContext        *p_v_codecCtx;
    PktQueueStruct         *pkt_queue_struct;
    PktQueueHead          *p_v_pktQueueHead;
    VideoState               *video_states;

    int ret;

    
    video_states = (VideoState *)ptr;
    p_av_formatCtx = video_states->p_av_formatCtx;
    p_v_codecCtx = video_states->p_av_codecCtx[VIDEO_STREAM_INDEX];
    p_v_pktQueueHead = &video_states->pktQueue[VIDEO_STREAM_INDEX];


    while (1) {
       av_init_packet(&pkt);
       if (av_read_frame(p_av_formatCtx, &pkt) < 0) {
          av_err_log("av_read_frame fail!\n");
          break;
       }


       if (pkt.stream_index == video_states->streams_index[VIDEO_STREAM_INDEX]) {
           ret = pktQueueStruct_create(p_v_pktQueueHead, 
                                                           &pkt_queue_struct,
                                                           video_states->vPkt_queue_struct,  
                                                           &pkt);
        
            while (packetQueue_is_full(p_v_pktQueueHead)) { 
                 SDL_LockMutex(p_v_pktQueueHead->mutex);      
                 SDL_CondWait(p_v_pktQueueHead->cond, p_v_pktQueueHead->mutex);
                 SDL_UnlockMutex(p_v_pktQueueHead->mutex);
            }    
            packetQueue_put(p_v_pktQueueHead, pkt_queue_struct);
            SDL_LockMutex(p_v_pktQueueHead->mutex); 
            SDL_CondSignal(p_v_pktQueueHead->cond);
            SDL_UnlockMutex(p_v_pktQueueHead->mutex);

        }

       else if (pkt.stream_index == video_states->streams_index[AUDIO_STREAM_INDEX])  {

        
       }
     
       else if (pkt.stream_index == video_states->streams_index[SUBTILE_STREAM_INDEX]) {

       }

       av_free_packet(&pkt);
     
    };

    return 0;
}


//the thread decode video packet
static int decode_video_thread(void *ptr)
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
        
        while(packetQueue_is_empty(p_v_pktQueueHead)) {
            SDL_LockMutex(p_v_pktQueueHead->mutex);
            SDL_CondWait(p_v_pktQueueHead->cond, p_v_pktQueueHead->mutex);
            SDL_UnlockMutex(p_v_pktQueueHead->mutex);
        }

        if (packetQueue_get(p_v_pktQueueHead, &pkt_queue_struct) >= 0) {
           get_video_frame(p_v_codecCtx, p_v_frame, &got_frame, (const AVPacket *)&pkt_queue_struct->packet);
           pktQueueStruct_destroy(p_v_pktQueueHead, pkt_queue_struct);
           SDL_LockMutex(p_v_pktQueueHead->mutex);
           SDL_CondSignal(p_v_pktQueueHead->cond);           
           SDL_UnlockMutex(p_v_pktQueueHead->mutex);
           
           if (got_frame) {
              av_info_log("decode a frame success!\n");

              ret = frameQueueStruct_create(p_v_frameQueueHead,
                                            &fQueueStruct, 
                                            video_states->vFrame_queue_struct, 
                                            p_v_frame);
              
              while (frameQueue_is_full(video_states->frameQueue) ) {
                  SDL_LockMutex(p_v_frameQueueHead->mutex);
                  SDL_CondWait(p_v_frameQueueHead->cond, p_v_frameQueueHead->mutex);
                  SDL_UnlockMutex(p_v_frameQueueHead->mutex); 
              }

              frameQueue_put(p_v_frameQueueHead, fQueueStruct);
              SDL_LockMutex(p_v_frameQueueHead->mutex);
              SDL_CondSignal(p_v_frameQueueHead->cond);
              SDL_UnlockMutex(p_v_frameQueueHead->mutex); 

           } else {
              av_err_log("decode a frame fail!\n");
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
           
       }
       else if (video_states->p_av_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
          video_states->streams_index[AUDIO_STREAM_INDEX] = i;
           
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


    switch (codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            video_states->p_av_codecCtx[VIDEO_STREAM_INDEX] = p_av_codecCtx;
            video_states->p_av_codec[VIDEO_STREAM_INDEX] = p_av_codec;
            // open codec
            ret = avcodec_open2(p_av_codecCtx, p_av_codec,  NULL); 
            if (ret < 0) {
                av_err_log("open %s(id=%d) codec fail!\n", avcodec_get_name(codec_id), codec_id);
                return -1;
            }   
            av_info_log("find video decoder sucess! name=\"%s\",id=%d\n", avcodec_get_name(codec_id), codec_id);

            packetQueue_init(&video_states->pktQueue[VIDEO_STREAM_INDEX], 
                                         &video_packet_list, 
                                         MAX_VIDEO_PACKET_QUEUE_LEN);
            packetStruct_init(video_states);
           
            frameQueue_init(&video_states->frameQueue[VIDEO_STREAM_INDEX], 
                                        &video_frame_list,
                                        MAX_VIDEO_FRAME_QUEUE_LEN);
            frameStruct_init(video_states);
                
            video_states->p_av_frame[VIDEO_STREAM_INDEX] = av_frame_alloc();
            if (!video_states->p_av_frame[VIDEO_STREAM_INDEX]) {
               av_err_log("alloc frame fail at %d\n", __LINE__);
               return -1;
            }

#ifdef SDL_VERSION_LIBSDL1P2_DEV
            video_states->read_pkt_tid = SDL_CreateThread(read_packet_thread, video_states);
            video_states->decode_video_tid = SDL_CreateThread(decode_video_thread, video_states);

            video_states->surface_top = SDL_SetVideoMode(p_av_codecCtx->width, p_av_codecCtx->height, 0, 0);
            if (!video_states->surface_top) {
               av_err_log("SDL SetVideoMode fail!\n");
               return -1;           
            }

            video_states->layer1 = SDL_CreateYUVOverlay(p_av_codecCtx->width, p_av_codecCtx->height, SDL_YV12_OVERLAY, video_states->surface_top);
            if (!video_states->layer1) {
                av_err_log("SDL CreateYUVOverlay fail!\n");
                return -1;           
            }

#elif defined(SDL_VERSION_LIBSDL2P0_DEV)
           SDL_CreateThread(read_packet_thread, "read packet", video_states);
           SDL_CreateThread(decode_video_thread, "video_decode", video_states);
                
#endif

           break;
        
     case AVMEDIA_TYPE_AUDIO:
          video_states->p_av_codecCtx[AUDIO_STREAM_INDEX] = p_av_codecCtx;
          video_states->p_av_codec[AUDIO_STREAM_INDEX] = p_av_codec;
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
       av_err_log("av_malloc buffer fail at %d, buffer_size=%d.\n", __LINE__, (buffer_size * sizeof(unsigned char)));   
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
    p_v_frame->format = AV_PIX_FMT_YUV420P;

    sprintf(path, "%s/%d.bmp", BMP_PATH, count);
    save_frame_to_bmp(path, p_v_frame->data[0], frame->width, frame->height, AV_PIX_FMT_RGB24);

    count++;
    av_free(p_buf);
    av_frame_free(&p_v_frame);  
    
    return 0;
}


static int videoImage_display(VideoState *video_states, AVFrame *frame)
{
    //AVPicture picture;
    SDL_Rect rect;

    SDL_LockYUVOverlay(video_states->layer1);
    
    video_states->layer1->pixels[0] = frame->data[0];
    video_states->layer1->pixels[1] = frame->data[1];
    video_states->layer1->pixels[2] = frame->data[2];

    video_states->layer1->pitches[0] = frame->linesize[0];
    video_states->layer1->pitches[1] = frame->linesize[1];
    video_states->layer1->pitches[2] = frame->linesize[2];

    rect.x = 0;
    rect.y = 0;
    rect.w = frame->width;
    rect.h = frame->height;


    SDL_DisplayYUVOverlay(video_states->layer1, &rect);
 
    SDL_UnlockYUVOverlay(video_states->layer1);
    
    return 1;
}

static int videoFrame_refresh(VideoState *video_states)
{
    AVFrame              *p_av_frame;
    FrameQueueStruct *frameQueue_struct;
    FrameQueueHead  *p_v_fqueue_head;

    p_v_fqueue_head = &video_states->frameQueue[VIDEO_STREAM_INDEX];    
    while (frameQueue_is_empty(p_v_fqueue_head)) {
        SDL_LockMutex(p_v_fqueue_head->mutex);
        SDL_CondWait(p_v_fqueue_head->cond, p_v_fqueue_head->mutex);
        SDL_UnlockMutex(p_v_fqueue_head->mutex);
    }

    frameQueue_get(p_v_fqueue_head, &frameQueue_struct);
    p_av_frame = frameQueue_struct->frame;
    //dump_YUV420P_frame(p_av_frame, 20);
    SDL_Delay(50);
    videoImage_display(video_states, p_av_frame);
    frameQueueStruct_destroy(p_v_fqueue_head, frameQueue_struct);
    SDL_LockMutex(p_v_fqueue_head->mutex);
    SDL_CondSignal(p_v_fqueue_head->cond);
    SDL_UnlockMutex(p_v_fqueue_head->mutex);

    return 0;
    
}

static int refresh_loopWait_event(VideoState *video_states, SDL_Event *event)
{
    SDL_PumpEvents();
    
#if defined(SDL_VERSION_LIBSDL1P2_DEV)
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_ALLEVENTS))
#elif defined(SDL_VERSION_LIBSDL2P0_DEV)
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
#endif
    {
        videoFrame_refresh(video_states);
    }

    SDL_PumpEvents();

    return 0;
}


static int event_loop(VideoState *video_states)
{
    SDL_Event event;

    while(1) {
       refresh_loopWait_event(video_states, &event);
       switch (event.type) {
           case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
                   case SDLK_LEFT:
                  exit(1);
                  break;
                
            }
       }
    }
}

#if 0
#define LOG_FILE_PATH          "/home/dlwang/ffplay.log"
static void log_sys_init(VideoState *video_states)
{
    video_states->fp_log  = fopen(LOG_FILE_PATH, "wr+");
    if (!video_states->fp_log ) {
        fprintf(stderr, "fopen log file fail!\n");
    }

}


static void log_sys_exit(VideoState *video_states)
{
    fclose(video_states->fp_log);
}

static void av_write_log(const char *fmt, ...)
{   
    va_list ap;

    SDL_LockMutex(g_video_state.mutex_log);
    g_video_state.fp_log  = fopen(LOG_FILE_PATH, "ar+");
    if (!g_video_state.fp_log ) {
        fprintf(stderr, "fopen log file fail!\n");
    }

    va_start(ap, fmt);
    vfprintf(g_video_state.fp_log, fmt, ap);
    va_end(ap);
    
    fclose(g_video_state.fp_log);
    SDL_UnlockMutex(g_video_state.mutex_log);
}
#endif

int main(int argc, char *argv[])
{

#if 0
    //log_sys_init(&g_video_state);

    g_video_state.fp_log = access(LOG_FILE_PATH, F_OK);
    
    if (g_video_state.fp_log == 0) {
       remove(LOG_FILE_PATH);
    }

    g_video_state.mutex_log = SDL_CreateMutex();
    init_player(&g_video_state);
#endif  
    get_input_param_list(&g_input_param, argc, argv);

    if (stream_open(&g_video_state, g_input_param.input_file) < 0)
       return -1;

    stream_component_open(&g_video_state, AVMEDIA_TYPE_VIDEO);

    if (SDL_Init(SDL_INIT_VIDEO| SDL_INIT_AUDIO| SDL_INIT_TIMER)) {
       av_err_log("could not initializ SDL-%s\n", SDL_GetError());
     return -1;
    }

     event_loop(&g_video_state);
    // log_sys_exit(&g_video_state);
    
} 
