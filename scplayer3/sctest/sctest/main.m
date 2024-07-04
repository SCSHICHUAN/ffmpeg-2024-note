//
//  main.m
//  sctest
//
//  Created by stan on 2024/7/3.
//
/*
 ++i：先增加，后返回
 i++：先返回，后增加
 */
#import <Foundation/Foundation.h>

typedef struct {
    int a;
    float b;
    char c[10];
} MyStruct;

typedef struct Frame{
    int rindex;
    int i;
}Frame;

void test1(void){
    MyStruct s;
        
        // 使用 memset 将结构体 s 的所有字节初始化为 0
        memset(&s, 0, sizeof(MyStruct));

        // 打印初始化后的结构体成员
        printf("s.a = %d\n", s.a);
        printf("s.b = %f\n", s.b);
        printf("s.c = %s\n", s.c);
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        // insert code here...
        NSLog(@"Hello, World!");
        Frame *fram;
        fram = malloc(sizeof(fram));
        fram->rindex = 0;
        
        
        for(int i= 0;i < 5;i++){
            printf("rindex++ = %d \n",fram->rindex++);
        }
        NSLog(@"");
        fram->rindex = 0;
        for(int i= 0;i < 5;i++){
            printf("++rindex = %d \n",++fram->rindex);
        }
        
        int i;
        printf("i = %d \n",i);
        printf("fram->i = %d \n",fram->i);
        
        test1();
    }
    return 0;
}


