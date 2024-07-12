//
//  ViewController.m
//  SCFFmpeg
//
//  Created by 石川 on 2019/5/18.
//  Copyright © 2019 石川. All rights reserved.
//

#import "ViewController.h"
#include "libavutil/log.h"
#include "libavformat/avio.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include <AVKit/AVKit.h>
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>
#import "SCRender.h"
#define kWidth ([UIScreen mainScreen].bounds.size.width)
#include "SCPlayer.h"
#import "SCAudioQueuePlayer.h"
#include "SCPlayer.h"

ViewController *c_self;

@interface ViewController ()  {
    SCRender *render;
}
@property(nonatomic,assign)BOOL end;
@property(nonatomic,strong)UILabel *lab;
@property(nonatomic,assign)NSInteger video_pak_count;
@property(nonatomic,assign)NSInteger audio_pak_count;
@property(nonatomic,strong)NSTimer *timer;
@end

@implementation ViewController
-(NSTimer *)timer{
    if(!_timer){
        _timer = [NSTimer scheduledTimerWithTimeInterval:1/60 target:self selector:@selector(update) userInfo:nil repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return _timer;
}


-(void)open{
    dispatch_async(dispatch_get_main_queue(), ^{
        self.lab.text = @"写入音频平数据中....";
        NSString *document = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSLog(@"document=%@",document);
    });
}
-(void)open2{
    dispatch_async(dispatch_get_main_queue(), ^{
        AVPlayerViewController *pvc = [[AVPlayerViewController alloc] init];
        NSString *document = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSString *path = [document stringByAppendingPathComponent:@"sc.mp4"];
        NSURL *url = [[NSURL alloc] initFileURLWithPath:path];
        pvc.player = [[AVPlayer alloc] initWithURL:url];
        [pvc.player play];
        [self presentViewController:pvc animated:YES completion:nil];
    });
}

int when_frame_push(AVFrame *frame, int flag,void *opaque){
    
    dispatch_async(dispatch_get_main_queue(), ^{
        
    if(flag == 0){
        [c_self initAudio:opaque];
    }else if(flag == 1){
       [c_self->render displayWithFrame:frame];
    }
    
    });
    
//    printf("fram.size = %d,flat = %d \n",frame->pkt_size,flag);
    return 0;
}


-(void)initAudio:(void *)opaque{
    VideoState *is = (VideoState *)opaque;
    SCAudioQueuePlayer *aup = [[SCAudioQueuePlayer alloc] init];
    [aup initializeAudioQueue:is];
    [aup play];
}






-(void)testClick2{
    self.end = NO;
    self.video_pak_count = 0;
    self.audio_pak_count = 0;
    self.lab.text = @"拉流中请稍等...";
    
//    dispatch_async(dispatch_get_global_queue(0, 0), ^{
//        [self run];
//    });
        dispatch_async(dispatch_get_global_queue(0, 0), ^{
            scplayer(when_frame_push);
        });
    
}
- (void)viewDidLoad {
    [super viewDidLoad];
    
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50, 100, kWidth-100, 40);;
        test.backgroundColor = UIColor.blueColor;
        [test setTitle:@"START 开始拉流" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick2) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
    [[UIApplication sharedApplication].keyWindow addSubview:test];
    test.frame = CGRectMake(50, 150, 100, 40);
    test.backgroundColor = UIColor.redColor;
    [test setTitle:@"前进" forState:UIControlStateNormal];
    [test addTarget:self action:@selector(testClick) forControlEvents:UIControlEventTouchDown];
    [test addTarget:self action:@selector(testClick) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:test];
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120, 150, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"后退" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick1) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick1) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"左" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick3) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick3) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"右" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick4) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick4) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120+105, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"右转" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick5) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick5) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    
    
    UILabel *lab = [[UILabel alloc] initWithFrame: CGRectMake(50, 250, kWidth-100, 40)];
    lab.backgroundColor = UIColor.blackColor;
    lab.textColor = UIColor.whiteColor;
    [self.view addSubview:lab];
    self.lab = lab;
    view = self.view;
    arry = [NSMutableArray array];
    count = 0;
    c_self = self;
   render = [[SCRender alloc] initWithFrame:CGRectMake(0, 300, kWidth, kWidth*(3/4.0))];
    [self.view addSubview:render];
    [self.timer fire];
}

bool forward_B;
bool back_B;

bool left_B;
bool right_B;

bool right_R_B;

-(void)testClick{
  forward_B = !forward_B;
}
-(void)testClick1{
    back_B = !back_B;
}

-(void)testClick3{
    left_B = !left_B;
}
-(void)testClick4{
    right_B = !right_B;
}
-(void)testClick5{
    right_R_B = !right_R_B;
}

-(void)update{
    if(forward_B){
        render.forward+=1;
    }
    if(back_B){
        render.back+=1;
    }
    if(left_B){
        render.left+=1;
    }
    if(right_B){
        render.right+=1;
    }
    if(right_R_B){
        render.right_R+=1;
    }
    
}


#define INBUF_SIZE 4096

#define WORD uint16_t
#define DWORD uint32_t
#define LONG int32_t

int wellDone;
#pragma pack(2)
UIView *view;
NSMutableArray *arry;
int count;
int stride = 2;
int stride_big = 0;
bool onec = YES;


static int decode_write_frame(AVFrame *frame)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [c_self->render displayWithFrame:frame];
    });
    return 0;
}







@end
