/*
Create by stan 2024-6-29
*/

#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL.h>

#define AUDIO_BUFFER_SIZE 1024

// SDL 窗口的大小
static int w_width = 720;
static int w_height = 480;

static SDL_Window *win = NULL;
static SDL_Renderer *renderer = NULL;

typedef struct _MyPacketEle
{
    AVPacket *pkt;
} MyPacketEle;

typedef struct _PacketQueue
{
    AVFifo *pkts;     // 储存元素
    int nb_packets;   // 元素的个数
    int size;         // 整个queue的大小
    int64_t duration; // 整个queue的时长

    SDL_mutex *mutex; // 互斥锁
    SDL_cond *cond;   // 信号量

} PacketQueue;

typedef struct _VideoState
{
    // 音频
    AVCodecContext *aCtx;
    AVPacket *aPkt;
    AVFrame *aFrame;

    struct SwrContext *swr_ctx;

    uint8_t *audio_buf;  // 解码后到音频数据
    uint audio_buf_size; // 数据包的大小
    int audio_buf_index; // 游标

    PacketQueue audioq;

    // 视频
    AVCodecContext *vCtx;
    AVPacket *vPkt;
    AVFrame *vFrame;

    SDL_Texture *texture;
} VideoState;

/*
  自定义queue
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
    for (;;)
    {
        if (av_fifo_read(q->pkts, &mypkt, 1) >= 0)
        {
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt);
            av_packet_move_ref(pkt, mypkt.pkt); // 给出数据包
            av_packet_free(&mypkt.pkt);
            ret = 1;
            break;
        }
        else if (!block)
        { // 非阻塞直接返回
            ret = 0;
            break;
        }
        else
        {
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

static void reander(VideoState *is)
{

    // 分别渲染Y U V，并且告诉line size 对应 data[0],Y  data[1],U data[2],V
    SDL_UpdateYUVTexture(is->texture, NULL, is->vFrame->data[0], is->vFrame->linesize[0],
                         is->vFrame->data[1], is->vFrame->linesize[1],
                         is->vFrame->data[2], is->vFrame->linesize[2]);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, is->texture, NULL, NULL); // 后面两个参数是 纹理的渲染大小
    SDL_RenderPresent(renderer);
}

// 接受到一个一个AVPacket
static int decode(VideoState *is)
{
    int ret = -1;
    char buf[1024];

    ret = avcodec_send_packet(is->vCtx, is->vPkt);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
        goto __OUT;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(is->vCtx, is->vFrame);
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
    AVPacket pkt;

    for (;;)
    {
        if (packet_queue_get(&is->audioq, &pkt, 1) < 0)
        { // 队列中读取数据
            return -1;
        }

        ret = avcodec_send_packet(is->aCtx, &pkt);
        if (ret < 0)
        {
            av_log(is->aCtx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        /**
         鉴于解码器是异步的处理，通常解码线程处理中 avcodec_send_packet() 和 avcodec_receive_frame()
         也不是一对一的使用的，为了确保没有遗漏的解码帧，可以调用1次送入包，反复调用解码直到没有帧输出
         */
        while (ret >= 0)
        {
            ret = avcodec_receive_frame(is->aCtx, is->aFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break; // 如果是这两种错误退出到上层循环 继续读取数据
            }
            else if (ret < 0)
            {
                // 解码失败退出
                av_log(is->aCtx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            /*
             音频重采样 统一到 AV_SAMPLE_FMT_S16
             */
            // 初始化音频重采样上下文
            if (!is->swr_ctx)
            {
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->aCtx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
                if (is->aCtx->sample_fmt != AV_SAMPLE_FMT_S16)
                { // 需要重采样
                    swr_alloc_set_opts2(&is->swr_ctx,
                                        &out_ch_layout,        // 输出声道数
                                        AV_SAMPLE_FMT_S16,     // 输出采样深度
                                        is->aCtx->sample_rate, // 输出采样率
                                        &in_ch_layout,         // 输入省道数
                                        is->aCtx->sample_fmt,  // 输入采样深度
                                        is->aCtx->sample_rate, // 输入采样率
                                        0,
                                        NULL);
                    swr_init(is->swr_ctx);
                }
            }
            if (is->swr_ctx)
            {
                // 输入 拿到原始的数据 在音频AVFrame中 extended_data域 = data域
                const uint8_t **in = (const uint8_t **)is->aFrame->extended_data;
                int in_count = is->aFrame->nb_samples; // 采样个数
                // 输出
                uint8_t **out = &is->audio_buf;               // 重采样输出数据
                int out_count = is->aFrame->nb_samples + 512; // 输出采样个数 512 融于值
                int out_size = av_samples_get_buffer_size(NULL,
                                                          is->aFrame->ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                // 输出的音频包开辟空间 实际开辟的空间audio_buf_size，out_size想要分配空间的大小
                av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size);
                // 音频重采样
                len2 = swr_convert(is->swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);
                // data_size = （len2）采样个数 x （nb_channels）音频通道数 x 位深
                data_size = len2 * is->aFrame->ch_layout.nb_channels 
                * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }
            else
            {
                // 不需要采样
                is->audio_buf = is->aFrame->data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       is->aFrame->ch_layout.nb_channels,
                                                       is->aFrame->nb_samples,
                                                       is->aFrame->format,
                                                       1);
            }
            av_packet_unref(&pkt);
            av_frame_unref(is->aFrame);
            return data_size;
        }
    }
__OUT:
    return ret;
}

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{ // len 声卡需要的音频长度
    int len1 = 0;
    int audio_size = 0;

    VideoState *is = (VideoState *)userdata;

    while (len > 0)
    {

        if (is->audio_buf_index >= is->audio_buf_size)
        { // buf数据用完
            audio_size = audio_decode_frame(is);
            if (audio_size < 0)
            {                                           // 没有解码到数据
                is->audio_buf_size = AUDIO_BUFFER_SIZE; // 播放静默音
                is->audio_buf = NULL;
            }
            else
            { // 解码到数据
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0; // 游标在开始位置
        }

        /*
        如果缓冲区buffer还有数据
        */
        // 缓冲区数据没有读完 比较 需要的和实际的数据,用少的那一个
        len1 = is->audio_buf_size - is->audio_buf_index; // 当前缓冲区数据还剩余的
        if (len1 > len)
        {
            len1 = len;
        }

        if (is->audio_buf)
        {
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }
        else
        {
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
int main(int argc, char *argv[])
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
    AVCodecContext *aCtx = NULL;
    AVPacket *aPkt = NULL;
    AVFrame *aFrame = NULL;
    // 音频渲染相关
    SDL_AudioSpec wanted_spec, spec;

    /*
      视频参数
    */
    int vIdx = -1;
    AVStream *vInStream = NULL;
    const AVCodec *vDec = NULL;
    AVCodecContext *vCtx = NULL;
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

    /*
      视频解码初始化
    */
    // 5. 根据流中的codec_id, 获得解码器
    vInStream = fmtCtx->streams[vIdx];
    vDec = avcodec_find_decoder(vInStream->codecpar->codec_id);
    if (!vDec)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not find libx264 Codec");
        goto __END;
    }
    // 6. 创建解码器上下文
    vCtx = avcodec_alloc_context3(vDec);
    if (!vCtx)
    {
        av_log(NULL, AV_LOG_ERROR, "内存不足\n");
        goto __END;
    }
    // 7. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(vCtx, vInStream->codecpar);
    if (ret < 0)
    {
        av_log(vCtx, AV_LOG_ERROR, "不能拷贝解码参数到视频解码环境中!\n");
        goto __END;
    }

    // 8. 绑定解码器上下文
    ret = avcodec_open2(vCtx, vDec, NULL);
    if (ret < 0)
    {
        av_log(vCtx, AV_LOG_ERROR, "打开视频解码器失败: %s \n", av_err2str(ret));
        goto __END;
    }
    // 9. 根据视频的宽/高创建纹理
    video_width = vCtx->width;
    video_height = vCtx->height;
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
    aCtx = avcodec_alloc_context3(aDec);
    if (!aCtx)
    {
        av_log(NULL, AV_LOG_ERROR, "内存不足\n");
        goto __END;
    }
    // 7. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(aCtx, aInStream->codecpar);
    if (ret < 0)
    {
        av_log(aCtx, AV_LOG_ERROR, "不能拷贝解码参数到音频解码环境中!\n");
        goto __END;
    }
    // 8. 绑定解码器上下文
    ret = avcodec_open2(aCtx, aDec, NULL);
    if (ret < 0)
    {
        av_log(vCtx, AV_LOG_ERROR, "打开音频解码器失败: %s \n", av_err2str(ret));
        goto __END;
    }
    // 9. 初始化音频设备参数
    // 为音频设备设置参数
    wanted_spec.freq = aCtx->sample_rate; // 采样率
    wanted_spec.format = AUDIO_S16SYS;    // 有符号的16位
    wanted_spec.channels = aCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;                 // 静默音
    wanted_spec.samples = AUDIO_BUFFER_SIZE; // 采样个数
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void *)is;
    /*SDL_OpenAudio需要传入两个参数，一个是我们想要的音频格式。一个是最后实际的音频格式。
     这里的SDL_AudioSpec，是SDL中记录音频格式的结构体。
     &spec 告诉调用者实际的参数
     */
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to open audio device!\n");
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
    is->vCtx = vCtx;
    is->vPkt = vPkt;
    is->vFrame = vFrame;
    // 音频
    aFrame = av_frame_alloc();
    aPkt = av_packet_alloc();
    is->aCtx = aCtx;
    is->aPkt = aPkt;
    is->aFrame = aFrame;
    packet_queue_init(&is->audioq);

    // 10. 从多媒体文件中读取数据，进行解码
    while (av_read_frame(fmtCtx, pkt) >= 0)
    {
        if (pkt->stream_index == vIdx)
        {
            av_packet_move_ref(is->vPkt, pkt);
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

    is->vPkt = NULL;
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
    if (aFrame)
    {
        av_frame_free(&aFrame);
    }
    if (aPkt)
    {
        av_packet_free(&aPkt);
    }
    if (aCtx)
    {
        avcodec_free_context(&aCtx);
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
    if (vCtx)
    {
        avcodec_free_context(&vCtx);
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