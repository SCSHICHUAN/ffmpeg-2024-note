#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL.h>

#define AUDIO_BUFFER_SIZE 1024
// 函数文档说明 https://ffmpeg.org/doxygen/trunk/
//ffmpeg 编解码最新 https://zhuanlan.zhihu.com/p/345530242
typedef struct _MyPacketEle {//代理元素 持有AVPacket的指针，也可以把AVPacket的指针给AVFifo
    AVPacket *pkt;
}MyPacketEle;

typedef struct _PacketQueue {
    AVFifo *pkts; //储存元素
    int nb_packets; //元素个数
    int size; //整个queue的大小
    int64_t duration;//整个queue的时长

    SDL_mutex *mutex;//互斥所
    SDL_cond *cond;//信号量
} PacketQueue;

typedef struct _VideoState {
    //音频
    AVCodecContext *aCtx;
    AVPacket       *aPkt;
    AVFrame        *aFrame;

    struct SwrContext *swr_ctx;

    uint8_t        *audio_buf;//解码后的音频数据
    uint           audio_buf_size;//数据包的大小
    int            audio_buf_index;//游标

    //视频
    AVCodecContext *vCtx;
    AVPacket       *vPkt;
    AVFrame        *vFrame;

    SDL_Texture    *texture;

    PacketQueue    audioq;
}VideoState;

static int w_width  = 640;
static int w_height = 480;

static SDL_Window   *win = NULL;
static SDL_Renderer *renderer = NULL;
//初始化队列
static int packet_queue_init(PacketQueue *q){
    memset(q, 0, sizeof(PacketQueue));
    q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW);//AV_FIFO_FLAG_AUTO_GROW 如果空间不足自动增长
    if(!q->pkts){
        return AVERROR(ENOMEM);
    }

    q->mutex = SDL_CreateMutex();
    if(!q->mutex){
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if(!q->cond){
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int packet_queue_put_priv(PacketQueue *q, AVPacket *pkt){
    MyPacketEle mypkt;
    int ret;

    mypkt.pkt = pkt;

    ret = av_fifo_write(q->pkts, &mypkt, 1);//把代理元素保存到avfifo中
    if(ret < 0)
        return ret;
    
    q->nb_packets++;//队列中的包的数量
    q->size += mypkt.pkt->size + sizeof(mypkt);//队列的size
    q->duration += mypkt.pkt->duration;//队列总时长

    SDL_CondSignal(q->cond);//告诉等待的线程

    return 0;
}
//推入数据包
static int packet_queue_put(PacketQueue *q, AVPacket *pkt){
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if(!pkt1){
        av_packet_unref(pkt);
        return -1;
    }

    av_packet_move_ref(pkt1, pkt);//pkt的所有内容和值和引用技术给pkt1 后pkt恢复到原始状态

    SDL_LockMutex(q->mutex);
    //..
    ret = packet_queue_put_priv(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if(ret < 0){
        av_packet_free(&pkt1);
    }

    return ret;

}
/*
 block 是否阻塞的方式获取数据、
 AVPacket *pkt 外部需要赋值的数据包
 **/
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block){
    MyPacketEle mypkt;
    int ret;

    SDL_LockMutex(q->mutex);//加锁
    for(;;){
        if(av_fifo_read(q->pkts, &mypkt, 1) >=0 ){//一个非负数表示成功，一个负错误码表示失败
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt);
            q->duration -= mypkt.pkt->duration;
            av_packet_move_ref(pkt, mypkt.pkt);//给出数据包
            av_packet_free(&mypkt.pkt);//释放
            ret = 1;
            break;
        } else if (!block){
            ret = 0;//非阻塞直接返回
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);//阻塞等待
        }
    }
    SDL_UnlockMutex(q->mutex);//解锁

    return ret;
}
//清空队列
static void packet_queue_flush(PacketQueue *q){
    MyPacketEle mypkt;

    SDL_LockMutex(q->mutex);
    while(av_fifo_read(q->pkts, &mypkt, 1) >0 ){
        av_packet_free(&mypkt.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;

    SDL_UnlockMutex(q->mutex);
}
//消销毁队列
static void packet_queue_destroy(PacketQueue *q){
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkts);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void render(VideoState *is){

    SDL_UpdateYUVTexture(is->texture,
                         NULL,
                         is->vFrame->data[0], is->vFrame->linesize[0],
                         is->vFrame->data[1], is->vFrame->linesize[1],
                         is->vFrame->data[2], is->vFrame->linesize[2]);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, is->texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static int decode(VideoState *is){
    int ret = -1;
    char buf[1024];

    ret = avcodec_send_packet(is->vCtx, is->vPkt);
    if(ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to send frame to decoder!\n");
        goto __OUT;
    }

    while( ret >= 0){
        ret = avcodec_receive_frame(is->vCtx, is->vFrame);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
            ret = 0;
            goto __OUT;
        } else if( ret < 0) {
            ret = -1; //退出程序
            goto __OUT;
        }
        render(is);//解码到一帧马上渲染
    }
__OUT:
    return ret;
}

static int audio_decode_frame(VideoState *is){
    int ret;
    int len2;

    int data_size = 0;

    AVPacket pkt;

    for(;;){

        if(packet_queue_get(&is->audioq, &pkt, 1) < 0){//从队列中获取数据包
            return -1;
        }

        ret = avcodec_send_packet(is->aCtx, &pkt);
        if(ret < 0){
            av_log(is->aCtx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        /**
         鉴于解码器是异步的处理，通常解码线程处理中 avcodec_send_packet() 和 avcodec_receive_frame() 也不是一对一的使用的
         ，为了确保没有遗漏的解码帧，可以调用1次送入包，反复调用解码直到没有帧输出
         */
        while(ret >=0 ){
            ret = avcodec_receive_frame(is->aCtx, is->aFrame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){//如果是这两种错误退出到上层循环 继续读取数据
                break;
            } else if( ret < 0){
                //解码失败退出
                av_log(is->aCtx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            
            //判断音频参数是否和设备相同，否则需要重采样，重采样想改变的采样大小，统一到 AV_SAMPLE_FMT_S16
            //初始化音频重采样上下文
            if(!is->swr_ctx) {
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->aCtx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);

                if(is->aCtx->sample_fmt != AV_SAMPLE_FMT_S16){
                    swr_alloc_set_opts2(&is->swr_ctx,
                                &out_ch_layout,
                                AV_SAMPLE_FMT_S16,//输出的采样格式，和设备的格式一致
                                is->aCtx->sample_rate,//采样率大小
                                &in_ch_layout,//输入音频通的数
                                is->aCtx->sample_fmt,//输入采样格式
                                is->aCtx->sample_rate, //输入采样率
                                0, 
                                NULL);
                    swr_init(is->swr_ctx);
                }
            
            }

            if(is->swr_ctx){
                //extended_data 要重采样的音频数据，音频中 AVFrame extended_data 域和 data 只向同一个区域
                const uint8_t **in = (const uint8_t **)is->aFrame->extended_data;//需要重采样音频数据，在音频AVFrame中 extended_data域 = data域
                int in_count = is->aFrame->nb_samples;//重采样的个数
                uint8_t **out = &is->audio_buf;//重采样输出包
                int out_count = is->aFrame->nb_samples + 512;//重采样输出，audio_buf 采样个数 + 512（融于值 = 1024/2）

                int out_size = av_samples_get_buffer_size(NULL, is->aFrame->ch_layout.nb_channels, out_count, AV_SAMPLE_FMT_S16, 0);
                av_fast_malloc(&is->audio_buf, &is->audio_buf_size, out_size);//输出的音频包开辟空间 实际开辟的空间audio_buf_size，out_size想要分配空间的大小
                //音频重采样 swr_convert()
                len2 = swr_convert(is->swr_ctx,
                            out,
                            out_count,
                            in,
                            in_count);
                //data_size = （len2）采样个数 x （nb_channels）音频通道数 x 位深
                data_size = len2 * is->aFrame->ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }else {
                //不需要重采样的直接赋值
                is->audio_buf = is->vFrame->data[0];
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

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len){//len 声卡需要的音频长度
    
    int len1 = 0;
    int audio_size = 0;

    VideoState *is = (VideoState*)userdata;

    while(len > 0){
        
        /*
         如果缓冲区buffer数据已经拷贝到声卡用完
         */
        // audio_buf_index:游标  audio_buf_size:数据包大小
        if(is->audio_buf_index >= is->audio_buf_size) {//缓冲区数据读完了
            audio_size = audio_decode_frame(is);//解码音频队列中的数据  audio_size啊:读取到的数据大小
            if(audio_size < 0 ){//没有解码到数据
                is->audio_buf_size = AUDIO_BUFFER_SIZE;//播放静默音，设备默认缓冲区的大小1024
                is->audio_buf = NULL;
            }else {//解码到音频数据
                is->audio_buf_size = audio_size;//给当前数据包设置大小
            }
            is->audio_buf_index = 0;//设置游标的数据为开始位置
        }

        
        /*
         如果缓冲区buffer还有数据
         */
        //缓冲区数据没有读完
        len1 = is->audio_buf_size - is->audio_buf_index;//当前缓冲区数据还剩余的
        if(len1 > len) {
            len1 = len;
        }

        if(is->audio_buf){//audio_buf 的数据拷贝到声卡中（stream）
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        }else {
            //如果audio_buf中没有数据
            memset(stream, 0, len1);//如果没有就设置静默音频，len1 长度，全是设置为0， 1024字节的数据 AUDIO_BUFFER_SIZE
        }

        len -= len1; //实际消耗len1的数据
        stream += len1;// 从这个地方开始接受数据
        is->audio_buf_index += len1;//移动游标
    }
}

int main(int argc, char *argv[]){

    int ret = -1;
    int vIdx = -1, aIdx = -1;

    char *src = NULL;

    AVFormatContext *fmtCtx = NULL;
    AVStream *aInStream = NULL;
    AVStream *vInStream = NULL;

    const AVCodec *vDec = NULL;
    AVCodecContext *vCtx = NULL;

    const AVCodec *aDec = NULL;
    AVCodecContext *aCtx = NULL;

    AVPacket *pkt = NULL;

    AVPacket *aPkt = NULL;
    AVFrame *aFrame = NULL;

    AVPacket *vPkt = NULL;
    AVFrame *vFrame = NULL;

    SDL_Texture *texture = NULL;
    SDL_Event event;

    Uint32 pixformat = 0;
    int video_width = 0;
    int video_height = 0;

    VideoState *is = NULL;

    SDL_AudioSpec wanted_spec, spec;

    av_log_set_level(AV_LOG_DEBUG);

    //1. 判断输入参数
    if(argc < 2){ //argv[0], simpleplayer, argv[1] src 
        av_log(NULL, AV_LOG_INFO, "arguments must be more than 2!\n");
        exit(-1);
    }

    src = argv[1];

    is = av_mallocz(sizeof(VideoState));
    if(!is){
        av_log(NULL, AV_LOG_ERROR, "NO MEMORY!\n");
        goto __END;
    }

    //2. 初始化SDL，并创建窗口和Render
    //2.1
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf( stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    //2.2 creat window from SDL
    win = SDL_CreateWindow("Simple Player",
                           SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED,
                           w_width, w_height,
                           SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if(!win) {
        fprintf(stderr, "Failed to create window, %s\n",SDL_GetError());
        goto __END;
    }

    renderer = SDL_CreateRenderer(win, -1, 0);

    //3. 打开多媒体文件，并获得流信息
    if((ret = avformat_open_input(&fmtCtx, src, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));
        goto __END;
    }

    if((ret = avformat_find_stream_info(fmtCtx, NULL)) < 0 ){
         av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));
         goto __END;
    }

    //4. 查找最好的视频流
    for(int i =0; i < fmtCtx->nb_streams; i++){
        if(fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
           vIdx < 0) {
            vIdx = i;
           }

        if(fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && 
           aIdx < 0) {
            aIdx = i;
           }
        
        if(vIdx > -1 && aIdx > -1){
            break;
        }
    }

    if(vIdx == -1){
        av_log(NULL, AV_LOG_ERROR, "Could not find video stream!\n");
        goto __END;
    }

    if(aIdx == -1){
        av_log(NULL, AV_LOG_ERROR, "Could not find audio stream!\n");
        goto __END;
    }

    aInStream = fmtCtx->streams[aIdx];
    vInStream = fmtCtx->streams[vIdx];
   
    //5. 根据流中的codec_id, 获得解码器
    vDec = avcodec_find_decoder(vInStream->codecpar->codec_id);
    if(!vDec){
        av_log(NULL, AV_LOG_ERROR, "Could not find libx264 Codec");
        goto __END;
    }

    //6. 创建解码器上下文
    vCtx = avcodec_alloc_context3(vDec);
    if(!vCtx){
        av_log(NULL, AV_LOG_ERROR, "NO MEMRORY\n");
        goto __END;
    }
    //7. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(vCtx, vInStream->codecpar);
    if(ret < 0){
        av_log(vCtx, AV_LOG_ERROR, "Could not copyt codecpar to codec ctx!\n");
        goto __END;
    }

    //8. 绑定解码器上下文
    ret = avcodec_open2(vCtx, vDec , NULL);
    if(ret < 0) {
        av_log(vCtx, AV_LOG_ERROR, "Don't open codec: %s \n", av_err2str(ret));
        goto __END;
    }
    //9. 根据视频的宽/高创建纹理
    video_width = vCtx->width;
    video_height = vCtx->height;
    pixformat = SDL_PIXELFORMAT_IYUV;
    texture = SDL_CreateTexture(renderer,
                                pixformat,
                                SDL_TEXTUREACCESS_STREAMING,
                                video_width,
                                video_height);

    //10. 根据流中的codec_id, 获得解码器
    aDec = avcodec_find_decoder(aInStream->codecpar->codec_id);
    if(!vDec){
        av_log(NULL, AV_LOG_ERROR, "Could not find libx264 Codec");
        goto __END;
    }

    //6. 创建解码器上下文
    aCtx = avcodec_alloc_context3(aDec);
    if(!aCtx){
        av_log(NULL, AV_LOG_ERROR, "NO MEMRORY\n");
        goto __END;
    }
    //7. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(aCtx, aInStream->codecpar);
    if(ret < 0){
        av_log(aCtx, AV_LOG_ERROR, "Could not copyt codecpar to codec ctx!\n");
        goto __END;
    }

    //8. 绑定解码器上下文
    ret = avcodec_open2(aCtx, aDec , NULL);
    if(ret < 0) {
        av_log(aCtx, AV_LOG_ERROR, "Don't open codec: %s \n", av_err2str(ret));
        goto __END;
    }

    packet_queue_init(&is->audioq);

    aPkt = av_packet_alloc();
    aFrame = av_frame_alloc();

    pkt = av_packet_alloc();
    vPkt = av_packet_alloc();
    vFrame = av_frame_alloc();

    //填充 VideoState
    is->texture = texture;
    is->aCtx = aCtx;
    is->aPkt = aPkt;
    is->aFrame = aFrame;

    is->vCtx = vCtx;
    is->vPkt = vPkt;
    is->vFrame = vFrame;

    //为音频设备设置参数
    wanted_spec.freq = aCtx->sample_rate; //采样率
    wanted_spec.format = AUDIO_S16SYS; // 有符号的16位
    wanted_spec.channels = aCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;//静默音
    wanted_spec.samples = AUDIO_BUFFER_SIZE; //采样个数
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void*)is;
    /*SDL_OpenAudio需要传入两个参数，一个是我们想要的音频格式。一个是最后实际的音频格式。
     这里的SDL_AudioSpec，是SDL中记录音频格式的结构体。
     &spec 告诉调用者实际的参数
     */
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0 ){
        av_log(NULL, AV_LOG_ERROR, "Failed to open audio device!\n");
        goto __END;
    }

    SDL_PauseAudio(0);

    //10. 从多媒体文件中读取数据，进行解码
    while(av_read_frame(fmtCtx, pkt) >= 0) {
        if(pkt->stream_index == vIdx) {
            av_packet_move_ref(is->vPkt, pkt); //11. 对解码后的视频帧进行渲染
            decode(is);
        }else if(pkt->stream_index == aIdx){
            packet_queue_put(&is->audioq, pkt);//把音频的数据包保存到queue中
        } else {
            av_packet_unref(pkt);//如果即不是音频包也不是视频包，取消对数据包的引用
        }

        //12. 处理SDL事件
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                goto __QUIT;
                break;
            default:
                break;
        } 
    }

    is->vPkt = NULL;
    decode(is);

__QUIT:
    ret = 0;

__END:
    //13. 收尾，释放资源
    if(vFrame){
        av_frame_free(&vFrame);
    }

    if(vPkt){
        av_packet_free(&vPkt);
    }

    if(aFrame){
        av_frame_free(&aFrame);
    }

    if(aPkt){
        av_packet_free(&aPkt);
    }

    if(pkt){
        av_packet_free(&pkt);
    }

    if(aCtx){
        avcodec_free_context(&aCtx);
    }

    if(vCtx){
        avcodec_free_context(&vCtx);
    }

    if(fmtCtx){
        avformat_close_input(&fmtCtx);
    }

    if(win){
        SDL_DestroyWindow(win);
    }

    if(renderer){
        SDL_DestroyRenderer(renderer);
    }

    if(texture){
        SDL_DestroyTexture(texture);
    }

    if(is){
        av_free(is);
    }

    SDL_Quit();

    return ret;
}
/*
 c语言malloc怎么用:
     在C语言中，使用malloc 函数可以在动态存储区分配一个连续的内存空间，该函数的声明格式为：
     void* malloc(size_t size);
     其中，size 表示要分配的内存空间的大小，单位为字节。函数返回一个指向分配区开头位置的指针。
 下面是一个使用malloc 函数分配内存空间的示例代码：
 #include ‹stdio.h>
 #include ‹stdlib.h>
 int main(){
     
      int* p= （int*）malloc（sizeof（int）;// 分配一个整型变量大小的内存空间
      if (p == NULL) {
          printf（"内存分配失败"）;
          exit（1）;
      ｝
   
    *p=10;// 将分配的内存空间初始化为10
    printf（"分配的内存地址为：%p，值为：%d\n"，р, *р);
    free（p);//使用完后及时释放内存空间
    return 0;
 ｝
 */

