/*
Create by stan 2024-6-30
*/

/*
一.main 主线程中 --------线程 1 主线程
  1.判断url的合法性
  2.创建win 和renderer

二.调用stream_open()函数
  1.创建is结构体
  2.创建video，audio，frame 队列 
  3.创建线程“read_thread()”读包队列------线程2

三.在读包线程中
  1.打开多媒体 avformat_open_input() 
  2.for循环读取音视频包保存到，视频包队列，和音频包队列中
  3.调用 audio_open() 设置音频的播放的参数，和会调函数 sdl_audio_callback ----线程3
    系统创建一个线程，读队列中的数据给声卡，解码decode，解码后就直接播放
  4.调用 decode_thread () 创建视频解码线程，读取包解码decode,把解码到到Frame插入到Frame队列中，等待显示  ---线程4

四.音视频同步 ---线程1中 主线程
 */

#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL.h>

//sdl
#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define AUDIO_BUFFER_SIZE 1024

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

static const char *input_filename;
static const char *window_title;

static int default_width = 1080; //期望显示的宽
static int default_height = 720; //期望显示的高
static int screen_width  = 0;
static int screen_height = 0;

static SDL_Window      *win;
static SDL_Renderer    *renderer;



enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

static int av_sync_type = AV_SYNC_AUDIO_MASTER;

// SDL 窗口的大小
static int w_width = 720;
static int w_height = 480;

static SDL_Window *win = NULL;
static SDL_Renderer *renderer = NULL;

typedef struct MyPacketEle
{
    AVPacket *pkt;
} MyPacketEle;

typedef struct PacketQueue
{
    AVFifo *pkts;     // 储存元素
    int nb_packets;   // 元素的个数
    int size;         // 整个queue的大小
    int64_t duration; // 整个queue的时长

    SDL_mutex *mutex; // 互斥锁
    SDL_cond *cond;   // 信号量

} PacketQueue;

typedef struct Fram{
    AVFrame *frame;  //存储解码后的视频帧
    double pts;      //帧的呈现时刻
    double duration; //帧的持续时间
    int64_t pos;     //在pkt中的位置
    int width;       //帧的宽
    int height;      //帧的高
    int format;      //像素格式
    AVRational sar;  //帧的宽高比
}Frame;

typedef struct FrameQueue{
    Frame queue[FRAME_QUEUE_SIZE];//帧数组
    int  rindex;                  //读帧索引
    int  windex;                  //写帧索引
    int  size;                    //整个队列的大小
    int  abort;                   //队列终止标志
    SDL_mutex *mutex; //互斥
    SDL_cond  *cond;  //同步
}FrameQueue;


typedef struct VideoState{
    //文件头信息
    char *filename;
    AVFormatContext *ic;

    //音视频同步相关
    int av_sync_type;

    double          audio_clock;     //音频pts
    double          frame_timer;     //已经播放完成的视频的时间
    double          frame_last_pts;  //视频最后一次的pts
    double          frame_last_delay;//视频最后一次的delay

    double          video_clock;//预测视频帧的pts 视频时间，最后的frame的pts+推算的下一帧的pts
    double          video_current_pts;     //当前播报视频帧的pts
    double          video_current_pts_time;//当前播报视频帧的pts单位秒

    // 音频
    int             audio_index;     //音频流的index
    AVStream        *audio_st;       //音频流
    AVCodecContext  *audio_ctx;      //音频的解码环境
    PacketQueue     audioq;          //音频的队列
    uint8_t         *audio_buf;      // 解码后到音频数据
    unsigned int    audio_buf_size;  // 数据包的大小
    unsigned int    audio_buf_index; // 游标
    AVFrame         audio_frame;     //音频的frame
    AVPacket        audio_pkt;       //音频包
    uint8_t         *audio_pkt_data; //音频原始数据
    int             audio_pkt_size;
    struct SwrContext *audio_swr_ctx; //音频重采样

    // 视频
    int             video_index; //视频流的index
    AVStream        *video_st;  //视频流
    AVCodecContext  *video_ctx; //视频的解码环境
    PacketQueue     videoq;     //视频包的queue
    AVPacket        video_pkt;  //视频pkt
    struct SwsContext *sws_ctx; //视频重采样
    SDL_Texture     *texture;   //纹理
    FrameQueue      pictq;      //储存解码后的视频帧 
    int width, height, xleft, ytop;//视频在SDL窗口位置和大小
  
    //线程和退出
    SDL_Thread      *read_tid;  //读取数据线程
    SDL_Thread      *decode_tid;//解码线程
    int             quit;
}VideoState;

/*
  自定义pkt queue
 */
// 初始化队列
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW); // AV_FIFO_FLAG_AUTO_GROW 自动增长
    if (!q->pkts)
    {
        return AVERROR(ENOMEM);
    }

    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        return AVERROR(ENOMEM);
    }
    return 0;
}

// put 私有的方法
static int packet_queue_put_priv(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    MyPacketEle mypkt;

    mypkt.pkt = pkt;

    ret = av_fifo_write(q->pkts, &mypkt, 1); // 把pkt添加到fifo中 保存
    if (ret < 0)
    {
        return ret;
    }

    q->nb_packets++;                            // 队列中包数量加1
    q->size += mypkt.pkt->size + sizeof(mypkt); // 队列的size
    q->duration += mypkt.pkt->duration;         // 队列总时长

    SDL_CondSignal(q->cond); // 告诉等待线程

    return 0;
}

// put 把AVPacket 保存到队列中
static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    AVPacket *pkt1;

    pkt1 = av_packet_alloc();
    if (!pkt1)
    {
        av_packet_unref(pkt);
        av_packet_unref(pkt1);
        av_log(NULL, AV_LOG_ERROR, "内存不足!\n");
    }
    // pkt的所有内容和值和引用技术给pkt1 后pkt恢复到原始状态
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_priv(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0)
    {
        av_packet_free(&pkt1);
    }

    return ret;
}

// get 获取保存AVPacket blocl表示是否阻塞
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    int ret;
    MyPacketEle mypkt;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (av_fifo_read(q->pkts, &mypkt, 1) >= 0) {
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt);
            av_packet_move_ref(pkt, mypkt.pkt); // 给出数据包
            av_packet_free(&mypkt.pkt);
            ret = 1;
            break;
        }
        else if (!block){ // 非阻塞直接返回
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex); // 阻塞等待
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// 清空队列
static void packet_queue_flush(PacketQueue *q)
{
    MyPacketEle mypkt;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkts, &mypkt, 1) > 0)
    {
        av_packet_free(&mypkt.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;

    SDL_UnlockMutex(q->mutex);
}
// 销毁队列
static void packet_queue_destory(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkts);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

/*
  自定义Frame queue
  生产线程 decode 线程、
  消费线程是 渲染线程
 */
//初始化Frame queue
static int frame_queue_init(FrameQueue *fq,int max_size){
    int i;
    memset(fq,0,sizeof(FrameQueue));//所有字节设置为 0
    /*
     初始化线程标志
     */
    if(!(fq->mutex = SDL_CreateMutex())){
        av_log(NULL,AV_LOG_FATAL,"SDL_CreateMutex(): %s\n",SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if(!(fq->cond = SDL_CreateCond())){
        av_log(NULL,AV_LOG_FATAL,"SDL_CreateCond(): %s\n",SDL_GetError());
        return AVERROR(ENOMEM);
    }
    /*
    初始化数组
    */
   for(i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
       if(!(fq->queue[i].frame = av_frame_alloc()))
           return AVERROR(ENOMEM);

    return 0;       
}
//销毁Frame queue
static void frame_queue_destory(FrameQueue *fq){
    int i;
    for(int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++){
        Frame *vp = &fq->queue[i];
        //释放 AVFrame 结构中引用的所有数据（如内部缓冲区）
        av_frame_unref(vp->frame);
        /*
        释放 AVFrame 结构本身的内存，
        还会将 vp->frame 指针设置为 NULL，表示这个 AVFrame 已经被释放且不可再使用。
        */
       av_frame_free(&vp->frame);
       //销毁现在变量标志
       SDL_DestroyMutex(fq->mutex);
       SDL_DestroyCond(fq->cond);
    }
        
}
//帧队列终止
static void frame_queue_abort(FrameQueue *fq){
    SDL_LockMutex(fq->mutex);
    fq->abort = 1;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}
//唤醒等待的线程
static void frame_queue_signal(FrameQueue *fq){
    /*
    唤醒等待在条件变量 fq->cond 上的一个线程。
    如果有多个线程在等待这个条件变量，那么只有一个线程会被唤醒。
    */
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

/*
  解码线程调用
*/
//fq_1.获取当前写视频帧位置0，当前可储存AVFrame 的Frame
static Frame *frame_queue_peek_writable(FrameQueue *fq){
    SDL_LockMutex(fq->mutex);
    /*
    生产没有消费完，等待消费，Frame queue没有处理消费，
    因为一般在播放器中，生产>消费，不用担心消费过快
    1.这个设计没有考虑 消费>生产 ， 如果出现消费大于生产，就会黑屏因为没有视频帧，
    2.我看在ffplay中是考虑了一下这个情况 加了 frame_queue_peek_readable() 函数
    */
   while(fq->size >= VIDEO_PICTURE_QUEUE_SIZE && !fq->abort){
        SDL_CondWait(fq->cond,fq->mutex);//生产过剩 等待消费 等待队列有空间插入新帧
   }
   SDL_UnlockMutex(fq->mutex);

   if(fq->abort)
      return NULL;

    return &fq->queue[fq->windex];    
}
//fq_2.视频帧已经保存到帧队列0的位置，写的index跳到1的位置
static void frame_queue_push(FrameQueue *fq){
    if(++fq->windex >= VIDEO_PICTURE_QUEUE_SIZE)
        fq->windex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size++;
    SDL_CondSignal(fq->cond);//生产一个，发送消息给，等待消费的(本节中没有等待消费处理)
    SDL_UnlockMutex(fq->mutex);
}
/*
  渲染线程调用
*/
//fq_3.从帧队列中0的位置，读取Frame渲染
static Frame *frame_queue_peek(FrameQueue *fq){
    return &fq->queue[fq->rindex];
}
//fq_4.释放已经渲染位置的的帧，读帧的index调到1
static void fream_queue_pop(FrameQueue *fq){
    Frame *vp = &fq->queue[fq->rindex];
    av_frame_unref(vp->frame);//结构中引用的所有数据
    if(++fq->rindex >= VIDEO_PICTURE_QUEUE_SIZE)
       fq->rindex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);//消费了一个，发出同步消息，给生产，如果生产在等待可以开始工作了
    SDL_UnlockMutex(fq->mutex);
}


static void reander(VideoState *is)
{

    // 分别渲染Y U V，并且告诉line size 对应 data[0],Y  data[1],U data[2],V
    // SDL_UpdateYUVTexture(is->texture, NULL, is->vFrame->data[0], is->vFrame->linesize[0],
    //                      is->vFrame->data[1], is->vFrame->linesize[1],
    //                      is->vFrame->data[2], is->vFrame->linesize[2]);
    // SDL_RenderClear(renderer);
    // SDL_RenderCopy(renderer, is->texture, NULL, NULL); // 后面两个参数是 纹理的渲染大小
    // SDL_RenderPresent(renderer);
}

// 接受到一个一个AVPacket
static int decode(VideoState *is)
{
    int ret = -1;
    char buf[1024];

    // ret = avcodec_send_packet(is->video_ctx, is->vPkt);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
        goto __OUT;
    }

    while (ret >= 0)
    {
        // ret = avcodec_receive_frame(is->video_ctx, is->vFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            ret = 0;
            goto __OUT;
        }
        else if (ret < 0)
        {
            ret = -1;
            goto __OUT;
        }
        reander(is);
    }

__OUT:
    return ret;
}

static int audio_decode_frame(VideoState *is)
{
    int ret;
    int len2;
    int data_size = 0;
    // AVPacket pkt;

    for (;;){
         //从队列中读取数据
        if (packet_queue_get(&is->audioq, &is->audio_pkt, 1) < 0){ // 队列中读取数据
            av_log(NULL,AV_LOG_ERROR,"不能从音频包queue中读取取包!\n");
            is->quit = 1;
            break;
        }

        ret = avcodec_send_packet(is->audio_ctx, &is->audio_pkt);
        if (ret < 0){
            av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        /**
         鉴于解码器是异步的处理，通常解码线程处理中 avcodec_send_packet() 和 avcodec_receive_frame()
         也不是一对一的使用的，为了确保没有遗漏的解码帧，可以调用1次送入包，反复调用解码直到没有帧输出
         */
        while (ret >= 0){

            ret = avcodec_receive_frame(is->audio_ctx, &is->audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break; // 如果是这两种错误退出到上层循环 继续读取数据
            }
            else if (ret < 0){
                // 解码失败退出
                av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            /*
             音频重采样 统一到 AV_SAMPLE_FMT_S16
             */
            // 初始化音频重采样上下文
            if (!is->audio_swr_ctx){
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->audio_ctx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
                if (is->audio_ctx->sample_fmt != AV_SAMPLE_FMT_S16)
                { // 需要重采样
                    swr_alloc_set_opts2(&is->audio_swr_ctx,
                                        &out_ch_layout,        // 输出声道数
                                        AV_SAMPLE_FMT_S16,     // 输出采样深度
                                        is->audio_ctx->sample_rate, // 输出采样率
                                        &in_ch_layout,         // 输入省道数
                                        is->audio_ctx->sample_fmt,  // 输入采样深度
                                        is->audio_ctx->sample_rate, // 输入采样率
                                        0,
                                        NULL);
                    swr_init(is->audio_swr_ctx);
                }
            }

            if (is->audio_swr_ctx){
                // 输入 拿到原始的数据 在音频AVFrame中 extended_data域 = data域
                const uint8_t **in = (const uint8_t **)is->audio_frame.extended_data;
                int in_count = is->audio_frame.nb_samples; // 采样个数
                // 输出
                uint8_t **out = &is->audio_buf;               // 重采样输出数据
                int out_count = is->audio_frame.nb_samples + 512; // 输出采样个数 512 融于值
                int out_size = av_samples_get_buffer_size(NULL,
                                                          is->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                // 输出的音频包开辟空间 实际开辟的空间audio_buf_size，out_size想要分配空间的大小
                av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size);
                // 音频重采样
                len2 = swr_convert(is->audio_swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);
                // data_size = （len2）采样个数 x （nb_channels）音频通道数 x 位深
                data_size = len2 * is->audio_frame.ch_layout.nb_channels 
                * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }else{
                // 不需要采样
                is->audio_buf = is->audio_frame.data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       is->audio_frame.ch_layout.nb_channels,
                                                       is->audio_frame.nb_samples,
                                                       is->audio_frame.format,
                                                       1);
            }


            //计算音频已经播放的时间
            if(!isnan(is->audio_frame.pts)){
                //音频时间，当前这帧还没有拷贝到声卡，可说时间是在后面一点，如果现在播放的10ms的音频，这里可能是20ms
                is->audio_clock = is->audio_frame.pts * av_q2d(is->audio_st->time_base) 
                                  + is->audio_frame.nb_samples / is->audio_frame.sample_rate;
            }else{
                is->audio_clock = NAN;
            }
           
            //清空
            av_packet_unref(&is->audio_pkt);
            av_frame_unref(&is->audio_frame);
            return data_size;
        }
    }
__OUT:
    return ret;
}

/*
  len 声卡需要的音频长度
  实际可能不能给len 
 */
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{ 
    int len1 = 0;
    int audio_size = 0;

    VideoState *is = (VideoState *)userdata;

    while (len > 0){

        if (is->audio_buf_index >= is->audio_buf_size){ // buf数据用完
            audio_size = audio_decode_frame(is);
            if (audio_size < 0){   // 没有解码到数据
                is->audio_buf_size = AUDIO_BUFFER_SIZE; // 播放静默音
                is->audio_buf = NULL;
            }else{ // 解码到数据
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0; // 游标在开始位置
        }

        /*
        如果缓冲区buffer还有数据
        */
        // 缓冲区数据没有读完 比较 需要的和实际的数据,用少的那一个
        len1 = is->audio_buf_size - is->audio_buf_index; // 当前缓冲区数据还剩余的
        if (len1 > len){
            len1 = len;
        }

        if (is->audio_buf){
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }else{
            memcpy(stream, 0, len1);
        }

        len -= len1;                 // 实际消耗len1的数据
        stream += len1;              // 从这个地方开始接受数据
        is->audio_buf_index += len1; // 移动游标
    }
}

/*
1. 判断输入参数
2. 初始化SDL，并创建窗口和Render
2.1
2.2 creat window from SDL
3. 打开多媒体文件，并获得流信息
4. 查找最好的视频流
5. 根据流中的codec_id, 获得解码器
6. 创建解码器上下文
7. 从视频流中拷贝解码器参数到解码器上文中
8. 绑定解码器上下文
9. 根据视频的宽/高创建纹理
10. 从多媒体文件中读取数据，进行解码
11. 对解码后的视频帧进行渲染
12. 处理SDL事件
13. 收尾，释放资源
*/
int main2(int argc, char *argv[])
{

    int ret = -1;
    char *src = NULL;
    SDL_Event event;

    VideoState *is = NULL;

    // ffmpeg 参数
    AVFormatContext *fmtCtx = NULL;
    AVPacket *pkt = NULL;

    /*
      音频参数
    */
    int aIdx = -1;
    AVStream *aInStream = NULL;
    const AVCodec *aDec = NULL;
    AVCodecContext *audio_ctx = NULL;
    AVPacket *aPkt = NULL;
    AVFrame *audio_frame = NULL;
    

    /*
      视频参数
    */
    int vIdx = -1;
    AVStream *vInStream = NULL;
    const AVCodec *vDec = NULL;
    AVCodecContext *video_ctx = NULL;
    AVPacket *vPkt = NULL;
    AVFrame *vFrame = NULL;
    // 视频渲染相关
    int video_width = 0; // 实际视频的宽度
    int video_height = 0;
    SDL_Texture *texture = NULL;
    Uint32 pixformat = 0;

    av_log_set_level(AV_LOG_DEBUG);

    // 1. 判断输入参数
    if (argc < 2)
    {
        av_log(NULL, AV_LOG_INFO, "参数必须大于2个！\n");
        exit(-1);
    }

    src = argv[1];
    is = av_malloc(sizeof(VideoState));
    if (!is)
    {
        av_log(NULL, AV_LOG_ERROR, "内存不足！\n");
        goto __END;
    }

    // 2. 初始化SDL，并创建窗口和Render
    // 2.1
    if (SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "不能初始化 SDL - %s\n", SDL_GetError());
        return -1;
    }

    // 2.2 creat window from SDL
    win = SDL_CreateWindow("SCPlayer",
                           SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED,
                           w_width, w_height,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win)
    {
        fprintf(stderr, "创建SDL窗口失败, %s\n", SDL_GetError());
        goto __END;
    }
    renderer = SDL_CreateRenderer(win, -1, 0);

    // 3. 打开多媒体文件，并获得流信息
    if ((ret = avformat_open_input(&fmtCtx, src, NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));
        goto __END;
    }

    if ((ret = avformat_find_stream_info(fmtCtx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));
        goto __END;
    }

    // 4. 输入的流头中找到音视频流id
    for (int i = 0; i < fmtCtx->nb_streams; i++)
    {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            vIdx < 0)
        {
            vIdx = i;
        }

        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            aIdx < 0)
        {
            aIdx = i;
        }

        if (vIdx > -1 && aIdx > -1)
        {
            break;
        }
    }

    if (vIdx == -1)
    {
        av_log(NULL, AV_LOG_ERROR, "多媒体中没有找到视频流!\n");
        goto __END;
    }

    if (aIdx == -1)
    {
        av_log(NULL, AV_LOG_ERROR, "多媒体中没有找到音频流!\n");
        goto __END;
    }

   
    // 9. 根据视频的宽/高创建纹理
    video_width = video_ctx->width;
    video_height = video_ctx->height;
    pixformat = SDL_PIXELFORMAT_IYUV;
    texture = SDL_CreateTexture(renderer,
                                pixformat,
                                SDL_TEXTUREACCESS_STREAMING,
                                video_width,
                                video_height);

    /*
      音频解码初始化
    */
    // 5. 根据流中的codec_id, 获得解码器
    aInStream = fmtCtx->streams[aIdx];
    aDec = avcodec_find_decoder(aInStream->codecpar->codec_id);
    if (!vDec)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not find aac Codec");
        goto __END;
    }
    // 6. 创建解码器上下文
    audio_ctx = avcodec_alloc_context3(aDec);
    if (!audio_ctx)
    {
        av_log(NULL, AV_LOG_ERROR, "内存不足\n");
        goto __END;
    }
    // 7. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(audio_ctx, aInStream->codecpar);
    if (ret < 0)
    {
        av_log(audio_ctx, AV_LOG_ERROR, "不能拷贝解码参数到音频解码环境中!\n");
        goto __END;
    }
    // 8. 绑定解码器上下文
    ret = avcodec_open2(audio_ctx, aDec, NULL);
    if (ret < 0)
    {
        av_log(video_ctx, AV_LOG_ERROR, "打开音频解码器失败: %s \n", av_err2str(ret));
        goto __END;
    }
    

    SDL_PauseAudio(0); // 开始播放

    pkt = av_packet_alloc();
    /*
      VideoState 初始化
     */
    // 视频
    vFrame = av_frame_alloc();
    vPkt = av_packet_alloc();
    is->texture = texture;
    is->video_ctx = video_ctx;
    // is->vPkt = vPkt;
    // is->vFrame = vFrame;
    // 音频
    audio_frame = av_frame_alloc();
    aPkt = av_packet_alloc();
    is->audio_ctx = audio_ctx;
    // is->aPkt = aPkt;
    // is->audio_frame = audio_frame;
    packet_queue_init(&is->audioq);

    // 10. 从多媒体文件中读取数据，进行解码
    while (av_read_frame(fmtCtx, pkt) >= 0)
    {
        if (pkt->stream_index == vIdx)
        {
            // av_packet_move_ref(is->vPkt, pkt);
            // 11. 对解码后的视频帧进行渲染
            decode(is);
        }
        else if (pkt->stream_index == aIdx)
        {
            packet_queue_put(&is->audioq, pkt); // 把音频的数据包保存到queue中
        }
        else
        {
            av_packet_free(&pkt); // 没有用的就丢弃
        }

        // 12. SDL 事件监听
        SDL_PollEvent(&event); // SDL配套使用 不然不会渲染
        switch (event.type)
        {
        case SDL_QUIT:
            goto __QUIT;
            break;
        default:
            break;
        }
        av_packet_unref(pkt);
    }

    // is->vPkt = NULL;
    decode(is);

__QUIT:
    ret = 0;

__END:
    // 13. 收尾，释放资源

    if (pkt)
    {
        av_packet_free(&pkt);
    }
    // 音频
    if (audio_frame)
    {
        av_frame_free(&audio_frame);
    }
    if (aPkt)
    {
        av_packet_free(&aPkt);
    }
    if (audio_ctx)
    {
        avcodec_free_context(&audio_ctx);
    }
    // 视频
    if (vFrame)
    {
        av_frame_free(&vFrame);
    }
    if (vPkt)
    {
        av_packet_free(&vPkt);
    }
    if (video_ctx)
    {
        avcodec_free_context(&video_ctx);
    }

    if (fmtCtx)
    {
        avformat_close_input(&fmtCtx);
    }
    if (win)
    {
        SDL_DestroyWindow(win);
    }
    if (renderer)
    {
        SDL_DestroyRenderer(renderer);
    }
    if (texture)
    {
        SDL_DestroyTexture(texture);
    }
    if (is)
    {
        av_free(is);
    }

    SDL_Quit();

    return ret;
}

static int audio_open(void *opaque,
                      AVChannelLayout *wanted_channel_layout,
                      int wented_salple_rate){

    /*SDL_OpenAudio需要传入两个参数，一个是我们想要的音频格式。一个是最后实际的音频格式。
     这里的SDL_AudioSpec，是SDL中记录音频格式的结构体。
     &spec 告诉调用者实际的参数
     */
    SDL_AudioSpec wanted_spec, spec; 
    int wanted_nb_channels = wanted_channel_layout->nb_channels;                   
    /*9.初始化音频设备参数
      为音频设备设置参数
    */ 
    wanted_spec.freq = wented_salple_rate; // 采样率
    wanted_spec.format = AUDIO_S16SYS;    // 有符号的16位
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.silence = 0;                 // 静默音
    wanted_spec.samples = AUDIO_BUFFER_SIZE; // 采样个数
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void *)opaque;
    
    av_log(NULL,AV_LOG_INFO,
           "wanted spec: channels:%d,sample_fmt:%d,sanple_ret:%d \n",
           wanted_nb_channels,AUDIO_S16,wented_salple_rate);

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0){
        av_log(NULL, AV_LOG_ERROR, "打开音频设备失败!\n");
        return -1;
    }
    return spec.size;
}


int stream_component_open(VideoState *is,int stream_index){
    
    int ret =-1;

    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx = NULL;
    const AVCodec *codec = NULL;
    
    int sample_rate;
    AVChannelLayout ch_layout = {0, };

    AVStream *st = NULL;
    int codec_id;


    if(stream_index < 0 || stream_index >= ic->nb_streams){
        return -1;
    }

    /*
      视频解码初始化
    */
    st = ic->streams[stream_index];
    codec_id = st->codecpar->codec_id;
    // dc_1. 根据流中的codec_id, 获得解码器
    codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find Codec");
        goto __ERROR;
    }
    // dc_2. 创建解码器上下文
    avctx = avcodec_alloc_context3(codec);
    if (!avctx){
        av_log(NULL, AV_LOG_ERROR, "内存不足\n");
        goto __ERROR;
    }
    // dc_3. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(avctx,st->codecpar);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "不能拷贝解码参数到视频解码环境中!\n");
        goto __ERROR;
    }
    // dc_4. 绑定解码器和上下文
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "打开视频解码器失败: %s \n", av_err2str(ret));
        goto __ERROR;
    }

    switch (avctx->codec_type){
    case AVMEDIA_TYPE_AUDIO:
         sample_rate = avctx->sample_rate;
         ret = av_channel_layout_copy(&ch_layout,&avctx->ch_layout);
         if(ret < 0){
            goto __ERROR;
         }
         ret = audio_open(is,&ch_layout,sample_rate);
         if(ret < 0){
            av_log(NULL,AV_LOG_ERROR,"不能打开音频设备!\n");
            goto __ERROR;
         }

         is->audio_buf_size = 0;
         is->audio_buf_index = 0;
         is->audio_st = st;
         is->audio_index = stream_index;
         is->audio_ctx = avctx;

         //开始播放音频
         SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        /* code */
        break;
    case AVMEDIA_TYPE_UNKNOWN:
       av_log(avctx,AV_LOG_ERROR,"Other media type unknow - media_type = %d\n",avctx->codec_type); 
        break;    
    default:
        av_log(avctx,AV_LOG_INFO,"Other media type - media_type = %d\n",avctx->codec_type);  
        break;
    }




  ret =0;
  goto __END;
__ERROR:
  if(avctx){
    avcodec_free_context(&avctx);
  }
__END:
  return ret;
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
                                    int scr_xleft,int scr_ytop,
                                    int scr_width,int scr_height,
                                    int pic_width,int pic_height,
                                    AVRational pic_sar){
    //  pic_sar.num = 4,pic_sar.den = 3;
    AVRational aspect_ratio = pic_sar;
    int64_t width,height,x,y;
    // aspect_ratio 无效（小于等于 0:1），则将其设置为 1:1（即像素为正方形）。                                    
    if(av_cmp_q(aspect_ratio,av_make_q(0,1)) <= 0)
       aspect_ratio = av_make_q(1,1);
    /*
      假设一个视频帧的宽度为 720 像素，高度为 480 像素，SAR（或 PAR）为 4:3。
                                   
                                    宽度            720      4
       视频帧的宽高比（DAR）为：DAR = ------- × SAR = ------ x --- = 2
                                    高度            480      3
     */   
    aspect_ratio = av_mul_q(aspect_ratio,av_make_q(pic_width,pic_height));
     /* XXX: we suppose the screen has a 1.0 pixel ratio */
    /*
    高度添满
    查看宽度释放适合
    */
    height = scr_height;
    //av_rescale(int64_t a, int64_t b, int64_t c) a * b / c
    width = av_rescale(height,aspect_ratio.num,aspect_ratio.den) & ~1;
    if(width > scr_height){
       width = screen_width;
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
       height = av_rescale(width,aspect_ratio.den, aspect_ratio.num) & ~1;
    }   
   
   //计算frame x y
   x = (scr_width - width) / 2;
   y = (scr_height - height) /2;
   rect->x = scr_xleft + x;
   rect->y = scr_ytop + y;
   //计算frame的 宽高
   rect->w = FFMAX((int)width,1);
   rect->h = FFMAX((int)height,1);

}

static void set_default_window_size(int width,int height,AVRational sar){
    SDL_Rect rect;
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if(max_width == INT_MAX && max_height == INT_MAX)
       max_height = height;
    calculate_display_rect(&rect,0,0,max_width,max_height,width,height,sar);
    default_width = rect.w;
    default_height = rect.h;  
}

int read_thread(void *arg){
    Uint32 pixformat;
    int ret = -1;
    int video_index  = -1;
    int audio_index  = -1;

    VideoState *is = (VideoState*)arg;
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
    av_log(NULL, AV_LOG_ERROR, "多媒体文件必须包含视频和音频!\n");
    goto __ERROR;
  }

  //音视频编解码器初始化
  if(audio_index >= 0){
    stream_component_open(is,audio_index);
  }
  if(video_index >= 0 ){
     AVStream *st = ic->streams[video_index];
     AVCodecParameters *codecpar = st->codecpar;
     AVRational sar = av_guess_sample_aspect_ratio(ic,st,NULL);//猜测视频流或帧的采样长宽比
     //如果显示帧的显示区域大于期望的区域，就等于期望的区域
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

  //读取多媒体包保存在pkt queue中
  for(;;){
       
        if(is->quit){
            ret = -1;
            goto __ERROR;
        }

        //没有消费完循环等待10ms，queue满了
        if(is->audioq.size > MAX_QUEUE_SIZE ||
           is->videoq.size > MAX_QUEUE_SIZE){
            SDL_Delay(10);
            continue;
           }
        //从上下文中读取包   
        ret = av_read_frame(is->ic,pkt);
        if(ret < 0){
            if(is->ic->pb->error == 0){//no error; wait for user input
                /*
                如果还没有读取到包，等100毫秒在读
                */
                SDL_Delay(100);
                continue;
            }else{
                break;
            }
        }

        //pkt 保存到 pkt queue中
        if(pkt->stream_index == is->video_index){
            packet_queue_put(&is->videoq,pkt); //视频包保存到视频队列
        }else if(pkt->stream_index == is->audio_index){
            packet_queue_put(&is->audioq,pkt); //音频包保存到音频队列
        }else{
            av_packet_unref(pkt);              //取他类型的包丢弃
        }
  }

  /* all done - wait for it 如果读取完了 等待100ms*/
   while (!is->quit){
        SDL_Delay(100);
    }


__ERROR:
    if(pkt){
        av_packet_free(&pkt);
    }
    if(ret !=0 ){
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }

  return ret;
}

static void stream_close(VideoState *is){

}

static Uint32 sdl_refresh_timer_cb(Uint32 interval,void *opaque){
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}
static void schedule_refresh(VideoState *is,int delay){
    SDL_AddTimer(delay,sdl_refresh_timer_cb,is);
}
static VideoState *stream_open(const char* filename){

    VideoState *is;
    is = av_mallocz(sizeof(VideoState));
    if(!is){
        av_log(NULL,AV_LOG_FATAL,"内存不足！\n");
        return NULL;
    }

    is->audio_index = is->video_index =-1;
    is->filename = av_strdup(filename);//源字符串的内容复制到新分配的内存区域，并返回指向该区域的指针
    if(!is->filename){
        goto __ERROR;
    }

    is->ytop = 0;
    is->xleft = 0;
    
    //初始化packet queue
    if(packet_queue_init(&is->videoq) < 0 || packet_queue_init(&is->audioq) < 0){
        goto __ERROR;
    }
    /*
    初始化video frame queue 用于保存解码后的视频帧，
    ffplay中同时有音频的帧的queue这里为了简单,没有音频的
    */
   if(frame_queue_init(&is->pictq,VIDEO_PICTURE_QUEUE_SIZE) < 0){
     goto __ERROR;
   }

   is->av_sync_type = av_sync_type;
   is->read_tid = SDL_CreateThread(read_thread,"read_thread",is);
   if(!is->read_tid){
      av_log(NULL,AV_LOG_FATAL,"SDL_CreateThread():%s\n",SDL_GetError());
      goto __ERROR;
   }

   schedule_refresh(is,40);//开始刷新视频帧 开是40ms一次
   
   return is;
__ERROR:
  stream_close(is);
  return NULL;
}




void video_refresh_timer(void *userdata){

}

static void do_exit(VideoState *is){
    if(is){
        stream_close(is);
    }
    if(renderer)
       SDL_DestroyRenderer(renderer);
    if(win)
       SDL_DestroyWindow(win);
    SDL_Quit();
    av_log(NULL,AV_LOG_QUIET,"%s","");      
}

static void sdl_event_loop(VideoState *is){
    SDL_Event event;
    for(;;){
        SDL_WaitEvent(&event);
        switch(event.type){
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

int main(int argc,char* argv[]){
    int ret  = 0;
    int flags = 0;
    VideoState *is;

    av_log_set_level(AV_LOG_INFO);

    if(argc < 2){
        fprintf(stderr,"没有输入文件\n");
        exit(1);
    }

    input_filename = argv[1];

    flags = SDL_INIT_AUDIO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if(SDL_Init(flags)){
        av_log(NULL,AV_LOG_FATAL,"不能初始化SDL - %s\n",SDL_GetError());
        exit(1);
    }

    win = SDL_CreateWindow("scplayer",
                            SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED,
                            default_width,
                            default_height,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if(win){
       renderer = SDL_CreateRenderer(win,-1,0); 
    }                        
    if(!win || !renderer){
        av_log(NULL,AV_LOG_FATAL,"创建窗口和渲染失败\n");
        do_exit(NULL);
    }

    is = stream_open(input_filename);
    if(!is){
        av_log(NULL,AV_LOG_FATAL,"初始化VideoState失败\n");
        do_exit(NULL);
    }

    sdl_event_loop(is);//循环读取事件，主要是视频帧显示事件
}
/*

“.”和“->”的区别

如果是指针需要用“->”

struct Person {
   int age;
};

struct Person p1;
p1.age = 25;

struct Person *p2;
p2 = (struct Person*) malloc(sizeof(struct Person));
p2->age = 32;

 */