//
//  SCPlayer.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/8.
//  Copyright © 2024 石川. All rights reserved.
//

#ifndef SCPlayer_h
#define SCPlayer_h

#include <stdio.h>
#include <libavformat/avformat.h>

// 定义一个函数指针类型
typedef int (*frame_call_bacl)(AVFrame *, int);


int scplayer(frame_call_bacl fn);

#endif /* SCPlayer_h */
