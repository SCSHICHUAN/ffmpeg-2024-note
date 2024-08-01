//
//  SCRender.m
//  SCFFmpeg
//
//  Created by stan on 2024/1/17.
//  Copyright © 2024 石川. All rights reserved.
//

#import "SCRender.h"
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#import "Camera.h"




#import "JpegUtil.h"

// camera
Camera camera(glm::vec3(0.0f, 0.0f, 0.0f));


   

@interface SCRender ()
{
    GLKView *glView;
    dispatch_queue_t _display_rgb_queue;
    /// 顶点对象
    GLuint _VBO;
    GLuint _VAO;
    GLuint _yTexture;
    GLuint _uTexture;
    GLuint _vTexture;
    GLuint _glProgram;
    /// 顶点着色器
    GLuint _vertextShader;
    /// 片段着色器
    GLuint _fragmentShader;
    
    GLuint          _colorRenderBuffer;
    GLuint          _depthRenderBuffer;
    GLuint          _frameBuffer;
    CAEAGLLayer     *_eaglLayer;
}
@end

@implementation SCRender
-(void)setForward:(float)forward{
    _forward = forward;
    camera.ProcessKeyboard(FORWARD, _forward/100000000.0);
}

-(void)setBack:(float)back{
    _back = back;
    camera.ProcessKeyboard(BACKWARD, _back/100000000.0);
}
-(void)setLeft:(float)left{
    _left = left;
    camera.ProcessKeyboard(LEFT, _back/100000000.0);
}
-(void)setRight:(float)right{
    _right = right;
    camera.ProcessKeyboard(RIGHT, _back/100000000.0);
    
}
-(void)setRight_R:(float)right_R{
    _right_R = right_R;
    camera.ProcessMouseMovement(_right_R/1000000.0, 0);
}


-(instancetype)initWithFrame:(CGRect)frame{
    self = [super initWithFrame:frame];
    if(self){
        [self config];
    }
    return self;
}

-(void)config{
    [self _creatOpenGLContent];
    [self _setupOpenGLProgram];
    [self _setupOpenGL];
}

-(void)_creatOpenGLContent{
    glView = [[GLKView alloc] initWithFrame:self.bounds];
    glView.context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    [EAGLContext setCurrentContext:glView.context];
    [self addSubview:glView];
    _eaglLayer = glView.layer;
    [self setupFrameAndRenderBuffer];
}


#pragma mark - OpenGL
/// 编译着色器
- (GLuint)_compileShader:(NSString *)shaderName shaderType:(GLuint)shaderType {
    if(shaderName.length == 0) return -1;
    NSString *shaderPath = [[NSBundle mainBundle] pathForResource:shaderName ofType:@"glsl"];
    NSError *error;
    NSString *source = [NSString stringWithContentsOfFile:shaderPath encoding:NSUTF8StringEncoding error:&error];
    if(error) return -1;
    GLuint shader = glCreateShader(shaderType);
    const char *ss = [source UTF8String];
    glShaderSource(shader, 1, &ss, NULL);
    glCompileShader(shader);
    int  success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if(!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        printf("shader error msg: %s \n", infoLog);
    }
    return shader;
}
/// 初始化OpenGL可编程程序
- (BOOL)_setupOpenGLProgram {
    //    [self.openGLContext makeCurrentContext];
    [EAGLContext setCurrentContext:glView.context];
    _glProgram = glCreateProgram();
    _vertextShader = [self _compileShader:@"vertex" shaderType:GL_VERTEX_SHADER];
    _fragmentShader = [self _compileShader:@"yuv_fragment" shaderType:GL_FRAGMENT_SHADER];
    glAttachShader(_glProgram, _vertextShader);
    glAttachShader(_glProgram, _fragmentShader);
    glLinkProgram(_glProgram);
    GLint success;
    glGetProgramiv(_glProgram, GL_LINK_STATUS, &success);
    if(!success) {
        char infoLog[512];
        glGetProgramInfoLog(_glProgram, 512, NULL, infoLog);
        printf("Link shader error: %s \n", infoLog);
    }
    glDeleteShader(_vertextShader);
    glDeleteShader(_fragmentShader);
    NSLog(@"===着色器加载成功===");
    return success;
}



// 球体参数
int numSegments = 100;    // 经度数
int numLatitudes = 100;   // 纬度数
float radius = 0.5f;     // 半径

// 生成球体的经纬线顶点数据和纹理坐标
void createSphereData(std::vector<float>& vertices, std::vector<unsigned int>& indices, int numSegments, int numLatitudes, float radius) {
    for (int i = 0; i <= numLatitudes; ++i) {
        float phi = M_PI * i / numLatitudes; // 纬度角
        for (int j = 0; j <= numSegments; ++j) {
            float theta = 2.0f * M_PI * j / numSegments; // 经度角

            float x = radius * sin(phi) * cos(theta);
            float y = radius * sin(phi) * sin(theta);
            float z = radius * cos(phi);

            float u = static_cast<float>(j) / numSegments;
            float v = static_cast<float>(i) / numLatitudes;

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);
            vertices.push_back(u);
            vertices.push_back(v);

            if (i < numLatitudes && j < numSegments) {
                unsigned int first = (i * (numSegments + 1)) + j;
                unsigned int second = first + numSegments + 1;

                indices.push_back(first);
                indices.push_back(second);
                indices.push_back(first + 1);

                indices.push_back(second);
                indices.push_back(second + 1);
                indices.push_back(first + 1);
            }
        }
    }
}

void setupSphere(unsigned int &VAO, unsigned int &VBO, unsigned int &EBO, int numSegments, int numLatitudes, float radius) {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
     
    /// 顶点数据
    createSphereData(vertices, indices, numSegments, numLatitudes, radius);

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}




- (void)_setupOpenGL {
    [EAGLContext setCurrentContext:glView.context];

    // 生成和配置 VAO 和 VBO
    unsigned int  EBO;
    setupSphere(_VAO, _VBO, EBO, numSegments, numLatitudes, radius);

    // 设置 OpenGL 状态
    glEnable(GL_DEPTH_TEST);
    
    
    glGenTextures(1, &_yTexture);
    [self _configTexture:_yTexture];
    
    glGenTextures(1, &_uTexture);
    [self _configTexture:_uTexture];
    
    glGenTextures(1, &_vTexture);
    [self _configTexture:_vTexture];
    
    glBindVertexArray(0);
    
}
- (void)_configTexture:(GLuint)texture {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}



float rad = 0;
int first = 0;
//创建投影矩阵
glm::mat4  projection = glm::mat4(1.0f);//单位矩阵初始化
glm::mat4 view = glm::mat4(1.0f);

- (void)displayWithFrame:(AVFrame *)yuvFrame bb:(void (^)(BOOL success))completionBlock{
    
    //正方形的位置
    glm::vec3 cubePositions[] = {
        glm::vec3( 0.0f,  0.0f,  0.0f),
        glm::vec3( 2.0f,  5.0f, -15.0f),
        glm::vec3(-1.5f, -2.2f, -2.5f),
        glm::vec3(-3.8f, -2.0f, -12.3f),
        glm::vec3( 2.4f, -0.4f, -3.5f),
        glm::vec3(-1.7f,  3.0f, -7.5f),
        glm::vec3( 1.3f, -2.0f, -2.5f),
        glm::vec3( 1.5f,  2.0f, -2.5f),
        glm::vec3( 1.5f,  0.2f, -1.5f),
        glm::vec3(-1.3f,  1.0f, -1.5f)
    };
    
    
    
    int videoWidth = yuvFrame->width;
    int videoHeight = yuvFrame->height;
    
    if(videoWidth <= 0 || videoHeight<=0)
        return;
    
    dispatch_async(dispatch_get_main_queue(), ^{
        glEnable(GL_DEPTH_TEST);
        //glDepthFunc(GL_ALWAYS);
        //glDepthMask(GL_FALSE);
        
        glClearColor(0.2, 1.0, 1.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        //    glViewport(0, 0, self.frame.size.width, self.frame.size.height);
        
        
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, self->_yTexture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,videoWidth,videoHeight,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,yuvFrame->data[0]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "yTexture"), 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, self->_uTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, videoWidth / 2, videoHeight / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, yuvFrame->data[1]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "uTexture"), 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, self->_vTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, videoWidth / 2, videoHeight / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, yuvFrame->data[2]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "vTexture"), 2);
        
        
      
        projection = glm::perspective(glm::radians(camera.Zoom), (float)videoWidth / (float)videoHeight, 0.1f, 100.0f);
        // 相机/视图转换
        view = camera.GetViewMatrix();
       
        //导入数据到gpu绘制
        glBindVertexArray(self->_VAO);
    });
    
    
        //把模型放到世界中  计算每个对象的模型矩阵，并在绘图前将其传递给着色器
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, cubePositions[0]);//模型位置
        /*
         参数1 模型矩阵
         参数2 旋转角度
         参数3 旋转角度
         */
        model = glm::rotate(model, glm::radians(rad), glm::vec3(0.0f, 0.0f, 1.0f));//模型旋转
        
        dispatch_async(dispatch_get_main_queue(), ^{
           
            glUniformMatrix4fv(glGetUniformLocation(self->_glProgram,"projection"), 1, GL_FALSE,&projection[0][0]);
            glUniformMatrix4fv(glGetUniformLocation(self->_glProgram,"view"), 1, GL_FALSE,&view[0][0]);
            glUniformMatrix4fv(glGetUniformLocation(self->_glProgram,"model"), 1, GL_FALSE,&model[0][0]);
            
            glDrawElements(GL_TRIANGLES, numSegments * numLatitudes * 6, GL_UNSIGNED_INT, 0);
            [self->glView.context presentRenderbuffer:GL_RENDERBUFFER];
            //使用着色程序
            glUseProgram(self->_glProgram);
            completionBlock(YES);
        });
    rad += 1;
    
}

- (void)setupFrameAndRenderBuffer
{
    // Setup color render buffer
    glGenRenderbuffers(1, &_colorRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    [glView.context renderbufferStorage:GL_RENDERBUFFER fromDrawable:_eaglLayer];
    
    // Setup depth render buffer
    int width, height;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    
    
    glViewport(0, 0, width, height);
    
    // Create a depth buffer that has the same size as the color buffer.
    glGenRenderbuffers(1, &_depthRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    
    // Setup frame buffer
    glGenFramebuffers(1, &_frameBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);
    
    // Attach color render buffer and depth render buffer to frameBuffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, _colorRenderBuffer);
    
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, _depthRenderBuffer);
    
    // Set color render buffer as current render buffer
    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    
    // Check FBO satus
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        NSLog(@"Error: Frame buffer is not completed.");
        exit(1);
    }
}

@end
