#import "ViewController.h"
#import <AVFoundation/AVFoundation.h>

#define NUM_BUFFERS 3
#define BUFFER_SIZE 40960

@interface ViewController () {
    AudioQueueRef audioQueue;
    AudioQueueBufferRef audioQueueBuffers[NUM_BUFFERS];
    FILE *pcmFile;
    BOOL audioQueueStarted;
    AudioFileID audioFileID;
}

@end

@implementation ViewController


- (void)viewDidLoad {
    [super viewDidLoad];
    

    // 打开PCM文件
    NSString *filePath = [[NSBundle mainBundle] pathForResource:@"your_pcm_file" ofType:@"caf"];
    pcmFile = fopen([filePath UTF8String], "rb");
    if (!pcmFile) {
        NSLog(@"无法打开PCM文件");
        return;
    }
    
    
    OSStatus status;
    
//    /// 读取文件, 获取文件基本数据格式
//    status = AudioFileOpenURL((__bridge CFURLRef _Nonnull)([NSURL URLWithString:filePath]), kAudioFileReadPermission, 0, &audioFileID);
    
    // 设置音频格式
    AudioStreamBasicDescription audioFormat = {
        .mSampleRate         = 44100,
        .mFormatID           = kAudioFormatLinearPCM,
        .mChannelsPerFrame   = 1,
        .mFormatFlags        = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBitsPerChannel     = 16,
        .mBytesPerPacket     = 2,
        .mBytesPerFrame      = 2,
        .mFramesPerPacket    = 1,
    };
    
//    // 赋值并获取audioFileBasicDescription
//    UInt32 ioDataSize = sizeof(audioFormat);
//    AudioFileGetProperty(audioFileID,  kAudioFilePropertyDataFormat, &ioDataSize, &audioFormat);
    
    
    
   
    // 创建AudioQueue
    status = AudioQueueNewOutput(&audioFormat, OutputBufferCallback, (__bridge void *)self, NULL, NULL, 0, &audioQueue);
    if (status != noErr) {
        NSLog(@"创建AudioQueue失败: %d", (int)status);
        return;
    }
    
    
    
    // 创建并分配音频缓冲区
    for (int i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(audioQueue, BUFFER_SIZE, &audioQueueBuffers[i]);
        if (status != noErr) {
            NSLog(@"分配AudioQueue缓冲区失败: %d", (int)status);
            AudioQueueDispose(audioQueue, true);
            return;
        }
        // 初始化填充音频数据到缓冲区
        OutputBufferCallback((__bridge void *)self, audioQueue, audioQueueBuffers[i]);
    }
    
    // 开始播放
    status = AudioQueueStart(audioQueue, NULL);
    if (status != noErr) {
        NSLog(@"启动AudioQueue失败: %d", (int)status);
        AudioQueueDispose(audioQueue, true);
        return;
    }
    
    audioQueueStarted = YES;
    NSLog(@"AudioQueue已启动");
}





// AudioQueue回调函数
void OutputBufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    ViewController *vc = (__bridge ViewController *)inUserData;
    if (!vc->pcmFile) return;
    
    UInt32 bytesRead = (UInt32)fread(inBuffer->mAudioData, 1, BUFFER_SIZE, vc->pcmFile);
    printf("bytesRead:%d \n",bytesRead);
    if (bytesRead > 0) {
        inBuffer->mAudioDataByteSize = bytesRead;
        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    } else {
        // 如果文件读取结束，停止播放
        AudioQueueStop(inAQ, false);
        vc->audioQueueStarted = NO;
        NSLog(@"音频播放结束");
    }
}

// 清理资源
- (void)dealloc {
    if (pcmFile) {
        fclose(pcmFile);
    }
    if (audioQueue) {
        if (audioQueueStarted) {
            AudioQueueStop(audioQueue, true);
        }
        AudioQueueDispose(audioQueue, true);
    }
}

@end
