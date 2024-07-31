//
//  main.cpp
//  opengl环境
//
//  Created by Stan on 2022/12/8.
//

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "Camera.h"

// settings
const unsigned int SCR_WIDTH = 1000;
const unsigned int SCR_HEIGHT = SCR_WIDTH*3024.0/4032.0;

// camera
Camera camera(glm::vec3(0.0f, 0.0f, 3.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;

// timing
float deltaTime = 0.0f;    // 当前帧和上一帧之间的时间
float lastFrame = 0.0f;



// 生成球体的经纬线顶点数据
void createSphereVertices(float* vertices, int numSegments, int numLatitudes, int numLongitudes, float radius) {
    int vertexCount = 0;
    
    // 生成纬线
    for (int i = 0; i < numLatitudes; ++i) {
        float phi = M_PI * (i + 1) / (numLatitudes + 1); // 纬度角，从0到π
        for (int j = 0; j < numSegments; ++j) {
            float theta = 2.0f * M_PI * j / numSegments; // 经度角，从0到2π
            float x = radius * sin(phi) * cos(theta);
            float y = radius * sin(phi) * sin(theta);
            float z = radius * cos(phi);
            vertices[vertexCount * 3] = x;
            vertices[vertexCount * 3 + 1] = y;
            vertices[vertexCount * 3 + 2] = z;
            vertexCount++;
        }
    }
    
    // 生成经线
    for (int i = 0; i < numLongitudes; ++i) {
        float theta = 2.0f * M_PI * i / numLongitudes; // 经度角，从0到2π
        for (int j = 0; j < numSegments; ++j) {
            float phi = M_PI * j / (numSegments - 1); // 纬度角，从0到π
            float x = radius * sin(phi) * cos(theta);
            float y = radius * sin(phi) * sin(theta);
            float z = radius * cos(phi);
            vertices[vertexCount * 3] = x;
            vertices[vertexCount * 3 + 1] = y;
            vertices[vertexCount * 3 + 2] = z;
            vertexCount++;
        }
    }
}

void createVBOVAO(unsigned int &VAO, unsigned int &VBO, int numSegments, int numLatitudes, int numLongitudes, float radius) {
    int totalVertices = (numSegments * numLatitudes) + (numSegments * numLongitudes);
    float vertices[totalVertices * 3];
    
    createSphereVertices(vertices, numSegments, numLatitudes, numLongitudes, radius);
    
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

//纹理创建和加载
void loadTexture(unsigned int &texture1,unsigned int &texture2){
    //创建纹理
    //    unsigned int texture1;
    glGenTextures(1, &texture1);//“1”纹理数量
    glBindTexture(GL_TEXTURE_2D, texture1);
    //设置环绕
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    //设置过滤
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 加载并生成纹理
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(true); //翻转Y轴
    unsigned char *data = stbi_load("./src/pipi.png", &width, &height, &nrChannels, 0);
    if (data)
    {
        //加载纹理 GL_RGBA这个参数一定要设置对
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        //自动生成所有需要的多级渐远纹理
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else
    {
        std::cout << "Failed to load texture1" << std::endl;
    }
    stbi_image_free(data);
    
    
    //创建纹理
    //    unsigned int texture2;
    glGenTextures(1, &texture2);//“1”纹理数量
    glBindTexture(GL_TEXTURE_2D, texture2);
    //设置环绕
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    //设置过滤
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 加载并生成纹理
    stbi_set_flip_vertically_on_load(true);
    data = stbi_load("./src/box.png", &width, &height, &nrChannels, 0);
    if (data)
    {
        //加载纹理 GL_RGBA这个参数一定要设置对
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        //自动生成所有需要的多级渐远纹理
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else
    {
        std::cout << "Failed to load texture2" << std::endl;
    }
    stbi_image_free(data);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow *window);
int main(int argc, const char * argv[]) {
    
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
    
    
    //配置glfw
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    
    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH,SCR_HEIGHT, "sc window", NULL, NULL);
    if (window == NULL)
    {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    //当前上下文设置为glfw
    glfwMakeContextCurrent(window);
    
    //初始化glad管理OpenGL函数地址
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }
    
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);//窗口改变时回调
    glfwSetCursorPosCallback(window, mouse_callback);//鼠标移动
    glfwSetScrollCallback(window, scroll_callback);//鼠标滚轮
    // 告诉GLFW捕获我们的鼠标
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    
    
    
    unsigned int texture1 = 0,texture2 = 0;
    loadTexture(texture1, texture2);
    
    //要使用相对路径要设置一下Xcode
    Shader ourShader("./shaders/vertex.vs",
                     "./shaders/fragment.fs");
    
    unsigned int shaderProgram = ourShader.ID;
    ourShader.use();
    //片段作色器,输入位置
    ourShader.setInt("texture1", 0);
    ourShader.setInt("texture2", 5);
    
    
    // 设置球体参数
    int numSegments = 36;     // 经度数
    int numLatitudes = 18;    // 纬度数
    int numLongitudes = 18;   // 使用的经线数
    float radius = 1.0f;      // 半径
    
    // 计算顶点数组大小
    int totalVertices = numSegments * (numLatitudes + numLongitudes);
    float* vertices = new float[totalVertices * 3];
    
    // 生成球体的顶点数据
    createSphereVertices(vertices, numSegments, numLatitudes, numLongitudes, radius);
    
    // 创建 VBO 和 VAO
    unsigned int VAO, VBO;
    createVBOVAO(VAO, VBO, numSegments, numLatitudes, numLongitudes, radius);
    
    
    
    
    
    //glfw 保活
    while(!glfwWindowShouldClose(window))//glfwWindowShouldClose 关闭事件
    {
        
        
        /*每帧时间逻辑 硬件设备也好这个值也小，用时间来表示步长，这移动数度在每个硬件平衡
         eg：好的设备1s绘制两次 deltaTime = 0.5，差的设备绘制一次 deltaTime = 1;
         cameraPos = 1s * (绘制次数) * （自己定义的常亮d）* deltaTime
         好的设备  cameraPos = 1s * 2 * d * 0.5 = 1d
         差的设备  cameraPos = 1s * 1 * d * 1.0 = 1d
         */
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        
        processInput(window);
        //深度测试
        glEnable(GL_DEPTH_TEST);
        //清除深度缓冲
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        glClear(GL_COLOR_BUFFER_BIT);//清空颜色
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);//清空色
        
        
        //绑定多个纹理赋值 GL_TEXTURE0....15，上面还要告诉，片段着色器位置
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture1);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, texture2);
        
        ourShader.use();
        
        
        
        //创建投影矩阵
        glm::mat4 projection    = glm::mat4(1.0f);//单位矩阵初始化
        projection = glm::perspective(glm::radians(camera.Zoom), (float)SCR_WIDTH / (float)SCR_HEIGHT, 0.1f, 100.0f);
        ourShader.setMat4("projection", projection);
        
        
        // 相机/视图转换
        glm::mat4 view = glm::mat4(1.0f);
        view = camera.GetViewMatrix();
        ourShader.setMat4("view", view);
        
        
        
        
        
        //绘制
        glUseProgram(shaderProgram);
        glBindVertexArray(VAO);
        
        
        for (unsigned int i = 0; i < 10; i++)
        {
            //把模型放到世界中  计算每个对象的模型矩阵，并在绘图前将其传递给着色器
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, cubePositions[i]);//模型位置
            float angle = 20.0f * i;
            model = glm::rotate(model, glm::radians(angle), glm::vec3(1.0f, 0.3f, 0.5f));//模型旋转
            ourShader.setMat4("model", model);
            
            // 绘制每一条纬线
            for (int i = 0; i < numLatitudes; ++i) {
                glDrawArrays(GL_LINE_LOOP, i * numSegments, numSegments);
            }
            // 绘制每一条经线
            int offset = numLatitudes * numSegments;
            for (int i = 0; i < numSegments; ++i) {
                glDrawArrays(GL_LINE_STRIP, offset + i * (numLatitudes + 1), numLatitudes + 1);
            }
            
        }
        
        
        
        glfwSwapBuffers(window);//不断交换缓冲帧
        glfwPollEvents();//不段接受事件
        
    }
    glfwTerminate();//释放资源
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteProgram(shaderProgram);
    
    
    return 0;
}


void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
}
void processInput(GLFWwindow *window)
{
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.ProcessKeyboard(FORWARD, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.ProcessKeyboard(BACKWARD, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.ProcessKeyboard(LEFT, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.ProcessKeyboard(RIGHT, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_SPACE ) == GLFW_PRESS)
        camera.ProcessKeyboard(UPWARD, deltaTime);
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS)
        camera.ProcessKeyboard(DOWN, deltaTime);
}
void mouse_callback(GLFWwindow* window, double xposIn, double yposIn)
{
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
    
    if (firstMouse)
    {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }
    
    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top
    
    lastX = xpos;
    lastY = ypos;
    
    camera.ProcessMouseMovement(xoffset, yoffset);
}
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    camera.ProcessMouseScroll(static_cast<float>(yoffset));
}
