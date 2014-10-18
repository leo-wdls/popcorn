#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/log.h"

#define TAG	"[ffplay]  "

#define __DEBUG_LOG__ 1
#if defined(__DEBUG_LOG__)
#define av_info_log(fmt, args...) printf(TAG fmt, ##args)
#else
#define av_info_log(fmt, args...)
#endif

#define av_err_log(fmt, args...)  printf(TAG fmt, ##args)


#define _4K_	(4*1024)

#if 0
#define BMP_PATH  "/data/test_xiaoxin"
#else
#define BMP_PATH  "/home/leo/test"
#endif

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

#if 0
typedef struct videoState {
    //AVFormatContext *pFormatCtx;
    AVCodecContext *p_V_CodecCtx;
    AVFrame *p_V_Frame;
    AVCodec *p_V_Codec;  
    //AVCodecParserContext *pCodecParserCtx;
}videoState;
struct audioState {};
#endif



/////////////////////////////////////////////////////////////////////////
// make build ok!
const char program_name[] = "ffplay_demon";
const int program_birth_year = 2014;

void show_help_default(const char *opt, const char *arg)
{
   return;
}
/////////////////////////////////////////////////////////////////////////

int save_bmp_file(char *path, void *data, struct image bmp)
{

   #define BF_TYPE_INDEX	0X00
   #define BF_SIZE_INDEX        0X02
   #define BF_RESERVER1_INDEX   0X06
   #define BF_RESERVER2_INDEX   0X08
   #define BF_OFFBITS_INDEX     0X0A
   #define BI_SIZE_INDEX        0X0E
   #define BI_WIDTH_INDEX       0X12
   #define BI_HEIGHT_INDEX      0X16
   #define BI_PLANES_INDEX      0X1A
   #define BI_BITCOUNT_INDEX    0x1C
   #define BI_COMPRESSION_INDEX 0X1E
   #define BI_SIZE_IMAGE_INDEX  0X22
   #define BI_XPPM_INDEX        0X26  //x pels per meter
   #define BI_YPPM_INDEX        0X2A  //y pels per meter
   #define BI_CLR_USED_INDEX    0X2E
   #define BI_CLR_IMPORTANT     0X32
   
   #define BMP_FILE_HEAD_SIZE	(14)
   #define BMP_INFO_HEAD_SIZE   (40)
 
   #define BMP_HEAD_SIZE	(BMP_FILE_HEAD_SIZE+BMP_INFO_HEAD_SIZE)

   
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

int save_frame_to_bmp(char *path, void *data, int width, int height, enum AVPixelFormat format)
{

   struct image bmp;

   bmp.width = width;
   bmp.height = height;

   if (format == AV_PIX_FMT_RGB24)
      bmp.format = BMP_FMT_RGB24;

   save_bmp_file(path, data, bmp);

   return 0;
}

int parseFrame_from_packet(AVCodecParserContext *p_av_codecParserCtx, const AVCodecContext *p_av_codecCtx, 
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

int get_video_frame(AVCodecContext *p_av_codecCtx, AVFrame *p_av_frame, int *got_frame, const AVPacket *p_av_packet)
{
   *got_frame = -1;

   if (p_av_codecCtx->codec->type == AVMEDIA_TYPE_VIDEO) {

       if (avcodec_decode_video2(p_av_codecCtx, p_av_frame, got_frame, p_av_packet) < 0) {
          av_err_log("video decode fail!\n");
          return -1;

       } else {

          return *got_frame;
       }
 
   } else {
       av_err_log("%d: the stream is not video streamIt but %d stream!\n", __FUNCTION__, p_av_codecCtx->codec->type);
       return -1;
   } 

}


int scale_YUV_to_RGB(unsigned char *src_data, int src_lineSize[], int src_W, int src_H, enum AVPixelFormat src_format,
                     unsigned char *dst_data, int dst_lineSize[], int dst_W, int dst_H, enum AVPixelFormat dst_format,
                     int sws_flag)
{
    struct SwsContext *p_swsCtx = NULL;

    if (!src_data || !dst_data)
       return 0;


    p_swsCtx = sws_getCachedContext(p_swsCtx,
                                   src_W, src_H, src_format,
                                   dst_W, dst_H, dst_format,
                                   sws_flag, NULL, NULL, NULL);

    sws_scale(p_swsCtx, (const unsigned char* const *)src_data, (const int *)src_lineSize, 0, src_H, dst_data, dst_lineSize);    
    av_info_log("frame format cover and scale!\n");
    //av_info_log("frame format cover and scale:\n");
    //av_info_log("src_W=%d, src_H=%d, src_format=%d, src_lineSize=%d, dst_W=%d, dst_H=%d, dst_format=%d, src_lineSize=%d\n", 
    //             src_W, src_H, src_format, src_lineSize, dst_W, dst_H, dst_format, dst_lineSize);

    return 0;
}



int main(int argc, char *argv[])
{

    int err;
    unsigned int i, j;
    int byte_cnt;
    int got_frame = 0;
    unsigned int video_stream = -1;
    unsigned char *buffer;
    char path[256];
    int max_picture_count;
    
    AVFormatContext *p_av_formatCtx = NULL;

    AVCodecContext *p_av_codecCtx = NULL;

    AVFrame *p_av_frame = NULL;

    AVFrame *p_av_frameRgb = NULL;

    AVCodec *p_av_codec = NULL;  


    AVPacket packet = {0}, out_pkt; 

    AVCodecParserContext *p_av_codecParserCtx = NULL;

    AVPicture pict;


    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 2) {
       av_info_log("argument number err!\n");
       return -1;
    }

    if(argc >= 3)
      max_picture_count = atoi(argv[2]);
    else
      max_picture_count = 5;

    // register all format of video file, and codec
    av_info_log("av register!\n");
    av_register_all();

    // open video file, get head info from file
    av_info_log("open input media file!\n");
    err = avformat_open_input(&p_av_formatCtx, argv[1], NULL, NULL);
    if (err < 0) {
       av_err_log("avformat_open_input open %s fail, err=%d\n",argv[1], err);
       return -1;
    }

    // get stream info(AVFormatContext)
    av_info_log("get stream info!\n");
    err = avformat_find_stream_info(p_av_formatCtx, NULL);
    if (err<0) {
       av_err_log("can not find codec parameter!\n");
       goto err1;        
    }

    // Dump information about file onto standard error
    av_dump_format(p_av_formatCtx, 0, argv[1], 0);

    av_info_log("nb_streams = %d\n", p_av_formatCtx->nb_streams);

    // get video stream
    for (i=0; i<p_av_formatCtx->nb_streams; i++) {
     	if (p_av_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
           video_stream = i;
	   break;

        }

    }

    if (video_stream == -1) {
       av_err_log("not find video stream!\n");
       goto err1;      
    }

    // get AVCodecContext(codec info)
    p_av_codecCtx = p_av_formatCtx->streams[video_stream]->codec;

    // get AVCodec(get codec)
    p_av_codec = avcodec_find_decoder(p_av_codecCtx->codec_id);
    if (!p_av_codec) {
       av_err_log("%s codec not support!\n", avcodec_get_name(p_av_codecCtx->codec_id));
       goto err1;        
    }

    // open codec
    err = avcodec_open2(p_av_codecCtx, p_av_codec, NULL); 
    if (err < 0) {
       av_err_log("open %s(id=%d) codec fail!\n", avcodec_get_name(p_av_codecCtx->codec_id), p_av_codecCtx->codec_id);
       goto err1;
    } 

    av_info_log("find video decoder sucess! name=\"%s\",id=%d\n", avcodec_get_name(p_av_codecCtx->codec_id), p_av_codecCtx->codec_id);

    p_av_frame = av_frame_alloc(); 
    if (!p_av_frame) {
       av_err_log("alloc frame fail at %d\n", __LINE__);
       goto err1;   
    }

    p_av_frameRgb = av_frame_alloc();
    if (!p_av_frameRgb) {
       av_err_log("alloc frame fail at %d\n", __LINE__);
       goto err2;
    }

    // caculate picture size
    byte_cnt = avpicture_get_size(AV_PIX_FMT_RGB24, p_av_codecCtx->width, p_av_codecCtx->height);

    av_info_log("a frame(RGB24) image size is %d\n", byte_cnt);

    // alloc a size memrry of picture
    buffer = (unsigned char *)av_malloc(byte_cnt * sizeof(unsigned char));
    if (!buffer) {
       av_err_log("alloc frame fail at %d\n", __LINE__);
       goto err2;
    }

    avpicture_fill((AVPicture *)p_av_frameRgb, buffer, AV_PIX_FMT_RGB24, p_av_codecCtx->width, p_av_codecCtx->height);
 
    // init AVCodecParsetContext struct
    p_av_codecParserCtx = av_parser_init(p_av_codecCtx->codec_id);

    av_init_packet(&packet);

    i = 0;

    do {
       av_free_packet(&packet);

       if (av_read_frame(p_av_formatCtx, &packet) < 0) {
          av_err_log("av_read_frame fail!\n");
          break;
       }

       if (p_av_formatCtx->streams[packet.stream_index]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
          get_video_frame(p_av_codecCtx, p_av_frame, &got_frame, (const AVPacket *)&packet);
          if (got_frame) {
            //av_info_log("decode a frame success!\n");
          }
          else {
            av_err_log("decode a frame fail!\n");
            continue;
          }

          pict.data[0] =  p_av_frame->data[0];
          pict.data[1] =  p_av_frame->data[2];
          pict.data[2] =  p_av_frame->data[1];
          pict.linesize[0] =  p_av_frame->linesize[0];
          pict.linesize[1] =  p_av_frame->linesize[2];
          pict.linesize[2] =  p_av_frame->linesize[1];

          scale_YUV_to_RGB(pict.data, pict.linesize, p_av_codecCtx->width, p_av_codecCtx->height, p_av_codecCtx->pix_fmt,
                           p_av_frameRgb->data, p_av_frameRgb->linesize, p_av_codecCtx->width, p_av_codecCtx->height, AV_PIX_FMT_RGB24,
                           SWS_BICUBIC);

          if (i < max_picture_count) {
             sprintf(path, BMP_PATH"/video_%d.bmp", i++);
             save_frame_to_bmp(path, p_av_frameRgb->data[0], p_av_codecCtx->width, p_av_codecCtx->height, AV_PIX_FMT_RGB24);

          } else {
             break;
          }

       } else {
         continue;

       }
              
    } while (1);


   av_free_packet(&packet);

   av_free(buffer);
   av_frame_free(&p_av_frame);
   avformat_close_input(&p_av_formatCtx);
   return 0;

err3:
   av_frame_free(&p_av_frame);
   av_free_packet(&packet);
   av_free(buffer);

err2:
   av_frame_free(&p_av_frame);

err1:
   avformat_close_input(&p_av_formatCtx);
   return -1;
}
 
