#include <SDL2/SDL.h>

#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define SDL_AUDIO_BUFFER_SIZE 1024 * 2

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

typedef struct MyAVPacketList {
    AVPacket *pkt;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct Frame {
    AVFrame *frame;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE]; //储存解码后的帧
    int rindex;          // 读取索引
    int windex;          // 写入索引
    int size;            // 队列中当前帧的数量
    int abort;           // 标志队列是否中止
    SDL_mutex *mutex;    // 互斥锁，用于线程同步
    SDL_cond *cond;      // 条件变量，用于线程同步
} FrameQueue;

typedef struct VideoState {

  //for multi-media file
  char            *filename;
  AVFormatContext *ic;

  //sync
  int             av_sync_type;

  double          audio_clock; ///< the time of have audio frame
  double          frame_timer; ///< the time of have played video frame 
  double          frame_last_pts;
  double          frame_last_delay;

  double          video_clock; ///预测视频帧的pts <pts of last decoded frame / predicted pts of next decoded frame
  double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
  int64_t         video_current_pts_time;  ///< sys time (av_gettime) at which we updated video_current_pts - used to have running video pts


  //for audio
  int             audio_index;
  AVStream        *audio_st;
  AVCodecContext  *audio_ctx;
  PacketQueue     audioq;
  uint8_t         *audio_buf;      // 解码后到音频数据
  unsigned int    audio_buf_size;  // 数据包的大小
  unsigned int    audio_buf_index; // 游标
  AVFrame         audio_frame;
  AVPacket        audio_pkt;
  uint8_t         *audio_pkt_data;
  int             audio_pkt_size;
  struct SwrContext *audio_swr_ctx;

  //for video
  int             video_index;
  AVStream        *video_st;
  AVCodecContext  *video_ctx;
  PacketQueue     videoq;
  AVPacket        video_pkt;
  struct SwsContext *sws_ctx;

  SDL_Texture     *texture;

  FrameQueue      pictq;

  int width, height, xleft, ytop;//视频在SDL窗口位置和大小
  
  SDL_Thread      *read_tid;
  SDL_Thread      *decode_tid;

  int             quit;

} VideoState;

static const char *input_filename;
static const char *window_title;

static int default_width = 1080;
static int default_height = 720;
static int screen_width  = 0;
static int screen_height = 0;

static SDL_Window      *win;
static SDL_Renderer    *renderer;

static int is_full_screen = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static int av_sync_type = AV_SYNC_AUDIO_MASTER;

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if (!q->pkt_list)
        return AVERROR(ENOMEM);
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList pkt1;
    int ret;

    pkt1.pkt = pkt;

    ret = av_fifo_write(q->pkt_list, &pkt1, 1);
    if (ret < 0)
        return ret;
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
        av_packet_free(&pkt1);

    return ret;
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            av_packet_move_ref(pkt, pkt1.pkt);
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList pkt1;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
        av_packet_free(&pkt1.pkt);
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static int frame_queue_init(FrameQueue *fq, int max_size)
{
    int i;
    memset(fq, 0, sizeof(FrameQueue));
    if (!(fq->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(fq->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
        if (!(fq->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destory(FrameQueue *fq)
{
    int i;
    for (i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++) {
        Frame *vp = &fq->queue[i];
        av_frame_unref(vp->frame);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(fq->mutex);
    SDL_DestroyCond(fq->cond);
}
//1.获取当前写视频帧位置0，当前可储存AVFrame 的Frame
static Frame *frame_queue_peek_writable(FrameQueue *fq)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(fq->mutex);
    /*
    生产没有消费完，等待消费，Frame queue没有处理消费，
    因为一般在播放器中，生产>消费，不用担心消费过快
    1.这个设计没有考虑 消费>生产 ， 如果出现消费大于生产，就会黑屏因为没有视频帧，
    2.我看在ffplay中是考虑了一下这个情况 加了 frame_queue_peek_readable() 函数
    */
    while (fq->size >= VIDEO_PICTURE_QUEUE_SIZE && !fq->abort) {
        SDL_CondWait(fq->cond, fq->mutex); 
    }
    SDL_UnlockMutex(fq->mutex);

    if (fq->abort)
        return NULL;
   
    return &fq->queue[fq->windex];
}
//2.视频帧已经保存到帧队列0的位置，写的index跳到1的位置
static void frame_queue_push(FrameQueue *fq)
{
    if (++fq->windex == VIDEO_PICTURE_QUEUE_SIZE)
        fq->windex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size++;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}
//3.从帧队列中0的位置，读取Frame渲染
static Frame *frame_queue_peek(FrameQueue *fq)
{
    return &fq->queue[fq->rindex];
}
//4.释放已经渲染位置的的帧，读帧的index调到1
static void frame_queue_pop(FrameQueue *fq)
{
    Frame *vp = &fq->queue[fq->rindex];
    av_frame_unref(vp->frame);
    if (++fq->rindex == VIDEO_PICTURE_QUEUE_SIZE)
        fq->rindex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);//消费了一个，发出同步消息，给生产，如果生产在等待可以开始工作了
    SDL_UnlockMutex(fq->mutex);
}
//等待的线程发送同步消息
static void frame_queue_signal(FrameQueue *fq)
{
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}
//帧队列终止
static void frame_queue_abort(FrameQueue *fq)
{
    SDL_LockMutex(fq->mutex);
    fq->abort = 1;//终止
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

double get_audio_clock(VideoState *is) {
  double pts;
  int hw_buf_size, bytes_per_sec, n;
  
  pts = is->audio_clock; /* maintained in the audio thread 在音频线程中维护*/
  hw_buf_size = is->audio_buf_size - is->audio_buf_index;// 当前缓冲区数据还剩余的
  bytes_per_sec = 0;
  n = is->audio_ctx->ch_layout.nb_channels * 2;//每个音频样本占用2个字节（通常是16位，等于2字节）。
  if(is->audio_st) {
    bytes_per_sec = is->audio_ctx->sample_rate * n;//44100 * 4;采样率 x 字节数 = 每秒的字节数
  }
  if(bytes_per_sec) {
    pts -= (double)hw_buf_size / bytes_per_sec;//时间pts = 缓冲的bytes/(每秒多少bytes)
  }
  return pts;
}
double get_video_clock(VideoState *is) {
  double delta;

  delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
  return is->video_current_pts + delta;
}
double get_external_clock(VideoState *is) {
  return av_gettime() / 1000000.0;
}

double get_master_clock(VideoState *is) {
  if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    return get_video_clock(is);
  } else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    return get_audio_clock(is);
  } else {
    return get_external_clock(is);
  }
}

//推算pts 因为有时会没有pts
double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {

  double frame_delay;

  if(pts != 0) {
    /* if we have pts, set video clock to it  */
    is->video_clock = pts;
  } else {
    /* if we aren't given a pts, set it to the clock */
    pts = is->video_clock;//如果这一帧没有pts，说明这一帧播放的时间不确定，把之前保存的video_clock给pts
  }
  /* update the video clock */
  frame_delay = av_q2d(is->video_ctx->time_base);// 1/24 = 0.0416 s
  /* if we are repeating a frame, adjust clock accordingly 
     重复帧用于平滑视频播放，并调整帧率
     重复帧（repeat_pict）是一个表示当前帧需要重复显示的次数的值，一般为0不重复
     帧率平滑：通过使用半帧时间的增加量，视频解码器可以更准确地分配时间戳，
     以平滑帧率转换。例如，从 24 FPS 转换到 30 FPS，或其他帧率转换。
  */
  frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
  is->video_clock += frame_delay;
  return pts;
}
/*
DAR（Display Aspect Ratio，显示长宽比）：视频显示的宽高比。
SAR（Sample Aspect Ratio，样本长宽比）：视频帧中每个像素的宽高比。
PAR（Pixel Aspect Ratio，像素长宽比）：与 SAR 等同，用于描述每个像素的宽高比。

在这种情况下，视频文件在显示设备上播放时，需要进行调整以匹配设备的像素特性，这可能会导致 SAR 和 PAR 的不一致。
假设有一个视频文件，其编码信息指示 SAR 为 1:1（即像素为正方形），
但该视频是为了在具有非正方形像素的显示设备上播放。在这种情况下，视频文件的 SAR 和实际显示设备的 PAR 可能会不一致。具体地：

视频文件的 SAR 为 1:1（正方形像素）
显示设备的 PAR 为 4:3（长方形像素）
在这种情况下，视频文件在显示设备上播放时，需要进行调整以匹配设备的像素特性，
这可能会导致 SAR 和 PAR 的不一致。

int scr_xleft：屏幕左上角的 x 坐标。
int scr_ytop：屏幕左上角的 y 坐标。
int scr_width：设置宽度。
int scr_height：设置的高度。
int pic_width：视频帧的宽度。
int pic_height：视频帧的高度。
AVRational pic_sar：视频帧的样本长宽比（SAR）。
 */
static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    //  pic_sar.num = 4,pic_sar.den = 3;
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;
   // aspect_ratio 无效（小于等于 0:1），则将其设置为 1:1（即像素为正方形）。
    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    /*
      假设一个视频帧的宽度为 720 像素，高度为 480 像素，SAR（或 PAR）为 4:3。
                                   
                                    宽度            720      4
       视频帧的宽高比（DAR）为：DAR = ------- × SAR = ------ x --- = 2
                                    高度            480      3
     */
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    /*
    高度添满
    查看宽度释放适合
    */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;//av_rescale(int64_t a, int64_t b, int64_t c) a * b / c
    if (width > scr_width) {
        width = scr_width;
        /*
        1.“~1” 是对整数 1 进行按位取反操作。在 32 位整数表示中，
        2.“1” 表示为 00000000 00000000 00000000 00000001，
        按位取反后的结果是 11111111 11111111 11111111 11111110，即 0xFFFFFFFE。

        3.按位与操作符 & 会对两个数的每一位执行与操作，即只有当两个数的对应位都是 1 时，结果才是 1，否则为 0。

        4.将 1923 与 0xFFFFFFFE 进行按位与操作：

                                  11110000011
           & 11111111111111111111111111111110
          --------------------------------------
                                  11110000010  
           11110000010 = 1922  
         5.原数的最低位变成了 0，即 1923 被转换为 1922，确保了结果是偶数,设备显示需要  
        */
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }

    printf("sar = %d/%d,aspect_ratio = %d/%d,d_width/d_height = %d/%d pic_w/pic_h = %d/%d \n",
    pic_sar.num,pic_sar.den,aspect_ratio.num,aspect_ratio.den,(int)width,(int)height,pic_width,pic_height);

    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = screen_width  ? screen_width  : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

int audio_decode_frame(VideoState *is) {

  int ret = -1;
  int data_size = 0;

  int len1, len2;

  for(;;) {

     //从队列中读取数据
    if(packet_queue_get(&is->audioq, &is->audio_pkt, 0) <= 0) {
      av_log(NULL, AV_LOG_ERROR, "Could not get packet from audio queue!\n");
      is->quit = 1;
      break;
    }

    ret = avcodec_send_packet(is->audio_ctx, &is->audio_pkt);
    av_packet_unref(&is->audio_pkt);
    if(ret < 0) {
      av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to decoder!\n");
      goto __OUT;
    }
  
    while(ret >= 0) {
      ret = avcodec_receive_frame(is->audio_ctx, &is->audio_frame);
      double pts_in_seconds = is->audio_frame.pts * av_q2d(is->audio_st->time_base);
      // printf("audio_frame pts = %lld %.2fs num/den=%d/%d \n",
      // is->audio_frame.pts,pts_in_seconds,is->audio_st->time_base.num,is->audio_st->time_base.den);


      if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
        break;
      } else if( ret < 0) {
        av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from decoder!\n");
        goto __OUT;
      }

      if(!is->audio_swr_ctx) {
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->audio_ctx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);

                if(is->audio_ctx->sample_fmt != AV_SAMPLE_FMT_S16){
                    swr_alloc_set_opts2(&is->audio_swr_ctx,
                                &out_ch_layout,
                                AV_SAMPLE_FMT_S16,
                                is->audio_ctx->sample_rate,
                                &in_ch_layout,
                                is->audio_ctx->sample_fmt,
                                is->audio_ctx->sample_rate, 
                                0, 
                                NULL);
                    swr_init(is->audio_swr_ctx);
                }
            
            }

      if(is->audio_swr_ctx){

        const uint8_t **in = (const uint8_t **)is->audio_frame.extended_data;
        uint8_t **out = &is->audio_buf;
        int out_count = is->audio_frame.nb_samples + 256;

	      //data_size = 2 * 2 * is->aframe->nb_samples;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_frame.ch_layout.nb_channels, out_count, AV_SAMPLE_FMT_S16, 0);
        av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size);

        //assert(data_size <= buf_size);
        len2 = swr_convert(is->audio_swr_ctx,
                    out,
                    out_count, //MAX_AUDIO_FRAME_SIZE*3/2,
                    in,
                    is->audio_frame.nb_samples);

        //输出
        data_size = len2 * is->audio_frame.ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
        //av_log(NULL, AV_LOG_DEBUG, "data_size:%d, compare:%d\n", data_size, 2 * 2 * is->audio_frame.nb_samples);
      }

      if (!isnan(is->audio_frame.pts))
        /*
        is->audio_clock = pts + 帧的持续时间
          当前播放调时刻  =  is->audio_clock - 音频帧剩余数据/bytes_per_sec
        */
        is->audio_clock = is->audio_frame.pts * av_q2d(is->audio_st->time_base)
                        + is->audio_frame.nb_samples / is->audio_frame.sample_rate;
      else
        is->audio_clock = NAN;
      
      // printf("audio_clock=%.f pkt=%.f  pts=%lld nb_samples=%d sample_rate=%d \n",
      //   is->audio_clock,is->audio_clock/1024,is->audio_frame.pts,is->audio_frame.nb_samples,is->audio_frame.sample_rate);

      //release pkt
      av_frame_unref(&is->audio_frame);  
      return data_size;
    }
  }

__OUT:
  return ret;
}

void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {

  VideoState *is = (VideoState *)userdata;
  int len1 = 0;
  int audio_size = 0;

  while(len > 0) {

    if(is->audio_buf_index >= is->audio_buf_size) {
      /* We have already sent all our data; get more */
      audio_size = audio_decode_frame(is);
      if(audio_size < 0) {
	      /* If error, output silence */
	      is->audio_buf_size = SDL_AUDIO_BUFFER_SIZE; 
        is->audio_buf = NULL;
      } else {
	      is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0;
    }

    len1 = is->audio_buf_size - is->audio_buf_index;
    if(len1 > len)
      len1 = len;

    if(is->audio_buf)
      memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    else 
      memset(stream, 0, len1);
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

static int video_open(VideoState *is)
{
    int w,h;

    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;

    if (!window_title)
        window_title = input_filename;
    SDL_SetWindowTitle(win, window_title);

    SDL_SetWindowSize(win, w, h);
    SDL_SetWindowPosition(win, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(win);

    is->width  = w;
    is->height = h;

    return 0;
}

static void video_display(VideoState *is){

  Frame *vp = NULL;
  AVFrame *frame = NULL;

  SDL_Rect rect;
  //1. open video
  if (!is->width)
        video_open(is);
  //2. peek frame
  vp = frame_queue_peek(&is->pictq);

  frame = vp->frame;

  //3. create texture
  if(!is->texture) {
    int width = frame->width;
    int height = frame->height;

    Uint32 pixformat= SDL_PIXELFORMAT_IYUV;

    //create texture for render
    is->texture = SDL_CreateTexture(renderer,
        pixformat,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height);
    if(!is->texture){
      av_log(NULL, AV_LOG_ERROR, "Failed to alloct texture, NO MEMORY!\n");
      return;
    }
  }
  //4. calculate rect
  // is->xleft = 100, is->ytop = 100,is->width = 100,is->height = 200;
  calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

  //5. render
  SDL_UpdateYUVTexture(is->texture, 
                      NULL, 
                      frame->data[0], frame->linesize[0],
                      frame->data[1], frame->linesize[1],
                      frame->data[2], frame->linesize[2]);

	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, is->texture, NULL, &rect);
  //SDL_RenderCopy(renderer, is->texture, NULL, NULL);
	SDL_RenderPresent(renderer); 
  //6. release frame
  frame_queue_pop(&is->pictq);
}

void video_refresh_timer(void *userdata) {

  VideoState *is = (VideoState *)userdata;
  Frame *vp = NULL;

  double actual_delay, delay, sync_threshold, ref_clock, diff;
  
  if(is->video_st) {
    if(is->pictq.size == 0) {
      /*
      if the queue is empty, so we shoud be as fast as checking queue of picture
      如果视频queue是空的，延时1毫秒 快速的检测
       */
      schedule_refresh(is, 1); 
    } else {
      /* Now, normally here goes a ton of code
	       about timing, etc. we're just going to
	       guess at a delay for now. You can
	       increase and decrease this value and hard code
	       the timing - but I don't suggest that ;)
	       We'll learn how to do it for real later.
         这里会有成顿的代码来，表示时间，看看是怎么做的
      */
      vp = frame_queue_peek(&is->pictq);//队列中取到要渲染的frame
      is->video_current_pts = vp->pts;//对is的域赋值，当前video的pts
      is->video_current_pts_time = av_gettime();//当前视频帧显示时间
      if(is->frame_last_pts == 0) {//一步是开始时 frame_last_pts 是为 0
        delay = 0;
      }else {
         /* the pts from last time，
         当前的pts - 上一帧的pts，判断过ms后播放现在的这一视频帧 */
        delay = vp->pts - is->frame_last_pts;
        // printf("video_pts-last_pts=%.4f ",delay);
      }
      
       /* 
       if incorrect delay, use previous one 
       如果 delay 的时间是<= 0 马上播放，如果
       dele >= 1s ，1s后播放，1s播放1帧这个不合理，我们就用上一次的delay
       */
      if(delay <= 0 || delay >= 1.0) {
        delay = is->frame_last_delay;
      }

      /* save for next time 
        这一就要展示了
        保存一下最后一次的delay 时间 和pts
        给上面计算 delay 使用
      */
      is->frame_last_delay = delay;
      is->frame_last_pts = vp->pts;

      /* update delay to sync to audio if not master source */
      if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {//视频同步到音频的模式
        ref_clock = get_master_clock(is);//音频audio_clock的时间
        diff = vp->pts - ref_clock;//视频时间 - 音频audio_clock的时间
        // printf("diff=%.4f ",diff);

        /* Skip or repeat the frame. Take delay into account
          FFPlay still doesn't "know if this is the best guess."
          
                sync_threshold   视频当前帧和下一帧的时间比较  视频自己的delay
                          diff   是视频时间和音频时间比较     视频和音频
          
           视频时间 - 音频audio_clock的时间     当前的pts - 上一帧的pts，
           vp->pts - ref_clock               vp->pts - is->frame_last_pts
                  |                                  |
                diff                               delay

            ---------------- 0 ---------------> x
              -3      -1           1       3
             diff   delay        delay   diff

             以diff为准来修正delay
           */
        sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;// 如果delay > 0.01 ? delay : 0.01
        if(fabs(diff) < AV_NOSYNC_THRESHOLD) {//如果diff绝对值 10 ms 小于这阀值说明音视频是同步的
         printf("fabs(diff)=%.4f ",fabs(diff));
          if(diff <= -sync_threshold) {//视频时间远远在前，视频要展示
            delay = 0;
          } else if(diff >= sync_threshold) {//视频是在后 ，并且delay 不够要修正
            delay = 2 * delay;
          }
        }
      }

      // printf("delay=%.4f ",delay);
      is->frame_timer += delay; //推算出下一帧的显示时间
      /* computer the REAL delay 
        一帧确定好系统时间后，后面就将要播放的帧的时间换算成系统时间，
        如果发现要播放的帧的时间落后于系统时间就将其播放出来。  
      */
      //查看这一帧是不是要显示，对比推算的时间和当前时间，如果推算的时间等于当前时间，立刻马上显示
      actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
      if(actual_delay < 0.010) {
        /* Really it should skip the picture instead */
        actual_delay = 0.010;
      }
      schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));
      printf("schedule_refresh_time= %dms\n",(int)(actual_delay * 1000 + 0.5));
      /* show the picture!
        展示当前帧
        上面的是计算下一帧展示时间准备的
       */
      video_display(is);
    }
  } else {
    schedule_refresh(is, 100);
  }
}
static int queue_picture(VideoState *is, 
                         AVFrame *src_frame, 
                         double pts, 
                         double duration, 
                         int64_t pos)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio; //SAR 表示单个像素的宽高比 SAR 为 1:1，则为方形像素

    vp->width = src_frame->width; //帧的宽
    vp->height = src_frame->height;//帧的高
    vp->format = src_frame->format; //像素格式 YUV420P、RGB24

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;

    //set_default_window_size(vp->width, vp->height, vp->sar);
    /*
    void av_frame_move_ref(AVFrame *dst, AVFrame *src) 
    是 FFmpeg 库中用于在两个 AVFrame 结构体之间移动引用数据的函数。
    这意味着将源帧 (src) 的数据和引用移动到目标帧 (dst)，同时清空源帧的数据。
    这在处理视频和音频帧时非常有用，因为它可以有效地管理和转移帧数据，而无需进行数据的深度复制。
     */
    av_frame_move_ref(vp->frame, src_frame);//移动解码后的帧到Frame queue的制定位置
    frame_queue_push(&is->pictq);
    return 0;
}

int video_decode_thread(void *arg) {

  int ret = -1;

  double pts;
  double duration;

  VideoState *is = (VideoState *)arg;
  AVFrame *video_frame = NULL;
  Frame *vp = NULL;

  AVRational tb = is->video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);//猜测视频流或帧的帧率（Frame Rate）

  video_frame = av_frame_alloc();

  for(;;) {
    if(is->quit) {
      break;
    }
    //从视频队列中获取视频包
    if(packet_queue_get(&is->videoq, &is->video_pkt, 0) <= 0) {//以非阻塞的方式从队列中获取包，如果没有就直接返回，并且等待10ms在取
      // means we quit getting packets
      av_log(is->video_ctx, AV_LOG_DEBUG, "video delay 10 ms\n");
      SDL_Delay(10);
      continue;
    }
    //发送视频包给解码器
    ret = avcodec_send_packet(is->video_ctx, &is->video_pkt);
    av_packet_unref(&is->video_pkt);//把包推给解码器后，他会拷贝一份，外部自己的引用计数清空，把包释放掉
    if(ret < 0) {
      av_log(is->video_ctx, AV_LOG_ERROR, "Failed to send pkt to video decoder!\n");
      goto __ERROR;
    }
    //轮询解码结果
    while(ret >=0) {
      ret = avcodec_receive_frame(is->video_ctx, video_frame);
      if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
        break;
      } else if( ret < 0) {
        av_log(is->video_ctx, AV_LOG_ERROR, "Failed to receive frame from video decoder!\n");
        ret = -1;
        goto __ERROR;
      }
      
      //video_display(is); //解码后直接展示，没有音视频同步

      //av sync
      duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);//视频帧持续的时间
      //0x8000000000000000 = 9223372036854775808 AV_NOPTS_VALUE 表示pts无效和没有初始化
      pts = (video_frame->pts == AV_NOPTS_VALUE) ? NAN : video_frame->pts * av_q2d(tb);//这里是以秒为单位的时间
      pts = synchronize_video(is, video_frame, pts);//计算video clock 视频的播放时长，当前的video_clock + 1/tbr(帧率)
      
      /*
       insert FrameQueue kt_pos: 这是一个 64 位的整数，
       用于表示帧在原始输入文件中的字节偏移量，
       它通常对应于该帧所在的压缩数据包（AVPacket）的文件位置
      */
      queue_picture(is, video_frame, pts, duration, video_frame->pkt_pos);

      //sub reference count
      av_frame_unref(video_frame);
    }
  }
  ret = 0;

__ERROR:
  av_frame_free(&video_frame);
  return ret;
}

static int audio_open(void *opaque, 
                      AVChannelLayout *wanted_channel_layout, 
                      int wanted_sample_rate){
  SDL_AudioSpec wanted_spec, spec;
  int wanted_nb_channels = wanted_channel_layout->nb_channels;
  
  // Set audio settings from codec info
  wanted_spec.freq = wanted_sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.silence = 0;
  wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = (void*)opaque;

  av_log(NULL, AV_LOG_INFO, 
        "wanted spec: channels:%d, sample_fmt:%d, sample_rate:%d \n",
        wanted_nb_channels, AUDIO_S16SYS, wanted_sample_rate);

  if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      av_log( NULL, AV_LOG_ERROR, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
  }
  return spec.size;
}


int stream_component_open(VideoState *is, int stream_index) {

  int ret = -1;

  AVFormatContext *ic = is->ic;
  AVCodecContext *avctx = NULL;
  const AVCodec *codec = NULL;

  int sample_rate;
  AVChannelLayout ch_layout = {0, };

  AVStream *st = NULL;
  int codec_id;

  if(stream_index < 0 || stream_index >= ic->nb_streams) {
    return -1;
  }

  st = ic->streams[stream_index];
  codec_id = st->codecpar->codec_id;
  //1. find decoder
  codec = avcodec_find_decoder(codec_id);
  //2. alloc condec context
  avctx = avcodec_alloc_context3(codec);
  if(!avctx){
    av_log(NULL, AV_LOG_ERROR, "Failed to alloc avctx, NO MEMORY!\n");
    goto __ERROR;
  }
  //3. copy parameter to codec context
  if((ret = avcodec_parameters_to_context(avctx, st->codecpar)) < 0) {
    av_log(avctx, AV_LOG_ERROR, "Couldn't copy codec parameters to codec context!\n");
    goto __ERROR; // Error copying codec context
  }
  //bind codec and codec context
  if((ret = avcodec_open2(avctx, codec, NULL))< 0) {
    av_log(NULL, AV_LOG_ERROR, "Failed to bind codecCtx and codec!\n");
    goto __ERROR;
  }

  switch(avctx->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    sample_rate = avctx->sample_rate;
    ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
    if(ret < 0) {
      goto __ERROR;
    }

    //open audio
    if((ret = audio_open(is, &ch_layout, sample_rate)) < 0) {
      av_log(avctx, AV_LOG_ERROR, "Failed to open audio device!\n");
      goto __ERROR;
    }

    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    is->audio_st = st;
    is->audio_index = stream_index;
    is->audio_ctx = avctx;
    
    //start play audio
    SDL_PauseAudio(0);

    break;

  case AVMEDIA_TYPE_VIDEO:
    is->video_index = stream_index;
    is->video_st = st;
    is->video_ctx = avctx;

    is->frame_timer = (double)av_gettime() / 1000000.0; //第一帧视频播放的时刻，加一个delay就是第二帧，加第二哥delay就是下一帧，以此类推
    is->frame_last_delay = 40e-3;//上一次渲染delay时间
    is->video_current_pts_time = av_gettime();//当前pts的系统时间

    //create decode thread 创建视频的解码线程
    is->decode_tid = SDL_CreateThread(video_decode_thread, "video_decode_thread", is);
   
    break;
  default:
    av_log(avctx, AV_LOG_ERROR, "Unknow Codec Type: %d\n", avctx->codec_type);
    break;
  }
  ret = 0;
  goto __END;
__ERROR:
  if(avctx){
    avcodec_free_context(&avctx);
  }
__END:
  return ret;
}

int read_thread(void *arg) {

  Uint32 pixformat;
  int ret = -1;

  int video_index = -1;
  int audio_index = -1;

  VideoState *is = (VideoState *)arg;
  AVFormatContext *ic = NULL;
  AVPacket *pkt = NULL;

  pkt = av_packet_alloc();
  if(!pkt){
    av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
    goto __ERROR;
  }

  //1. Open media file
  if((ret = avformat_open_input(&ic, is->filename, NULL, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not open file: %s, %d(%s)\n", is->filename, ret, av_err2str(ret));
    goto __ERROR; // Couldn't open file
  }
  is->ic = ic;
  
  //2. extract media info
  if(avformat_find_stream_info(ic, NULL) < 0) { 
    av_log(NULL, AV_LOG_FATAL, "Couldn't find stream information\n");
    goto __ERROR;
  }
  
  //3. Find the first audio and video stream
  for(int i = 0; i < ic->nb_streams; i++) {
    AVStream *st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    if(type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
      video_index=i;
    }
    if(type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
      audio_index=i;
    }

     //find video and audio
    if(video_index > -1 && audio_index > -1) {
      break;
    }
  }

  if(video_index < 0 || audio_index < 0) {
    av_log(NULL, AV_LOG_ERROR, "the file must be contains audio and video stream!\n");
    goto __ERROR;
  }

  if(audio_index >= 0) { //4. open audio part 打开音频流
    stream_component_open(is, audio_index);
  }
  if(video_index >= 0) { //5. open video part
    //设置视频在DSL窗口上的显示
    AVStream *st = ic->streams[video_index];
    AVCodecParameters *codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);//用于猜测视频流或帧的采样长宽比
    if (codecpar->width){
       if(codecpar->width <= default_width && codecpar->height <= default_height){
          set_default_window_size(codecpar->width, codecpar->height, sar);
       }else{
          set_default_window_size(default_width, default_height, sar);
       }
    }else{
      set_default_window_size(default_width, default_height, sar);
    }
        
    
    //打开视频流    
    stream_component_open(is, video_index);
  }   

  //main decode loop
  for(;;) {

    if(is->quit) {
      ret = -1;
      goto __ERROR;
    }

    /*
    limit queue size
    如果视频队列和音频队列满了，程序睡10毫秒，可持续睡
    */
    if(is->audioq.size > MAX_QUEUE_SIZE ||
       is->videoq.size > MAX_QUEUE_SIZE) {
      SDL_Delay(10);
      continue;
    }

    //6. read packet
    ret = av_read_frame(is->ic, pkt);
    if(ret < 0) {
      if(is->ic->pb->error == 0) {
        //如果没有读取到包，等100毫秒在读
        SDL_Delay(100); /* no error; wait for user input */
        continue;
      } else {
	      break;
      }
    }

    //7. save packet to queue
    if(pkt->stream_index == is->video_index) {
      packet_queue_put(&is->videoq, pkt);//视频包保存到视频队列
    } else if(pkt->stream_index == is->audio_index) {
      packet_queue_put(&is->audioq, pkt);//音频包保存到音频队列
    } else { //discard other packets 
      av_packet_unref(pkt);//取他类型的包丢弃
    }
  }

  /* all done - wait for it */
  while(!is->quit) {
    SDL_Delay(100);//如果用户退出等100毫秒退出
  }

  ret = 0;

 __ERROR:
  if(pkt) {
    av_packet_free(&pkt);
  }
  //推入SDL退出事件
  if(ret !=0 ){
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }

  return ret;
}

static void stream_component_close(VideoState *is, int stream_index){
  AVFormatContext *ic = is->ic;
  AVCodecParameters *codecpar;

  if (stream_index < 0 || stream_index >= ic->nb_streams)
      return;
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
      SDL_CloseAudio();
      swr_free(&is->audio_swr_ctx);
      av_freep(&is->audio_buf);
      is->audio_buf = NULL;

      break;
  case AVMEDIA_TYPE_VIDEO:
    frame_queue_abort(&is->pictq);
    frame_queue_signal(&is->pictq); //可以确保所有等待的线程都被唤醒
    SDL_WaitThread(is->decode_tid, NULL);
    is->decode_tid = NULL;
      break;
  default:
      break;
  }
}

static void stream_close(VideoState *is)
{
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_index >= 0)
        stream_component_close(is, is->audio_index);
    if (is->video_index >= 0)
        stream_component_close(is, is->video_index);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);

    frame_queue_destory(&is->pictq);

    av_free(is->filename);
    if(is->texture)
        SDL_DestroyTexture(is->texture);
    av_free(is);
}

static VideoState* stream_open(const char* filename){

  VideoState      *is;
  is = av_mallocz(sizeof(VideoState));
  if(!is){
    av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
    return NULL;
  }

  is->audio_index = is->video_index = -1;
  is->filename = av_strdup(filename);
  if(!is->filename){
      goto __ERROR;
  }
  is->ytop    = 0;
  is->xleft   = 0;

  //初始化packet queue
  if(packet_queue_init(&is->videoq) < 0 ||
      packet_queue_init(&is->audioq) < 0) {
      goto __ERROR;
      }
  
  //初始化video frame queue 用于保存解码后的视频帧，ffplay中同时有音频的帧的queue这里为了简单不用了
  if(frame_queue_init(&is->pictq, VIDEO_PICTURE_QUEUE_SIZE) < 0) {
    goto __ERROR;
  }

  //set sync type 
  is->av_sync_type = av_sync_type;

  //create an new thread for reading audio and video data
  is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
  if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto __ERROR;
  }

  //set timer for show picture
  schedule_refresh(is, 40);

  return is;

__ERROR:
  stream_close(is);
  return NULL;
}

static void do_exit(VideoState *is){
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (win)
        SDL_DestroyWindow(win);

    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sdl_event_loop(VideoState *is){
  SDL_Event       event;
  for(;;) {
    SDL_WaitEvent(&event);
    switch(event.type) {
      case FF_QUIT_EVENT:
      case SDL_QUIT:
        is->quit = 1;
        do_exit(is);
        break;
      case FF_REFRESH_EVENT:
        video_refresh_timer(event.user.data1);
        break;
      default:
        break;
    }
  }
}

int main(int argc, char *argv[]) {

  int 		  ret = 0;
  int       flags = 0;
  VideoState      *is;

  av_log_set_level(AV_LOG_INFO);

  if(argc < 2) {
    fprintf(stderr, "Usage: command <file>\n");
    exit(1);
  }

  //get filename
  input_filename = argv[1];

  flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if(SDL_Init(flags)) {
    av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }

  //creat window from SDL
  win = SDL_CreateWindow("Media Player",
                         SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED,
						             default_width, default_height,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if(win) {
    renderer = SDL_CreateRenderer(win, -1, 0);
  }

  if(!win || !renderer){
      av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer!\n");
      do_exit(NULL);
  }

  //import: open audio and video stream
  is = stream_open(input_filename);
  if (!is) {
      av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
      do_exit(NULL);
  }

  //listen key or mouse event
  sdl_event_loop(is);

  return ret;
}
