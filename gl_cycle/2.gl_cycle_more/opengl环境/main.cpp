//
//  main.cpp
//  opengl环境
//
//  Created by Stan on 2022/12/8.
//




#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "shader.h"



// 生成圆周的顶点数据
void createCircle(float* vertices, int numSegments, float radius, float z) {
    float angleStep = 2.0f * M_PI / numSegments;

    for (int i = 0; i < numSegments; ++i) {
        float angle = i * angleStep;
        vertices[i * 3] = radius * cos(angle);
        vertices[i * 3 + 1] = radius * sin(angle);
        vertices[i * 3 + 2] = z;
    }
}

// 生成球体的纬度线顶点数据
void createSphereLatitudes(float* vertices, int numSegments, int numLatitudes, float radius) {
    int vertexCount = 0;
    for (int i = 0; i < numLatitudes; ++i) {
        float phi = M_PI * (i + 1) / (numLatitudes + 1); // 纬度角度
        float z = radius * cos(phi);                    // z坐标
        float r = radius * sin(phi);                    // 纬线的半径

        createCircle(vertices + vertexCount * 3, numSegments, r, z);
        vertexCount += numSegments;
    }
}

// 创建 VAO 和 VBO，并绑定顶点数据
void createVBOVAO(unsigned int &VAO, unsigned int &VBO, int numSegments, int numLatitudes, float radius) {
    int totalVertices = numSegments * numLatitudes;
    float vertices[totalVertices * 3];
    createSphereLatitudes(vertices, numSegments, numLatitudes, radius);

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

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void processInput(GLFWwindow *window);

int main(int argc, const char * argv[]) {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(800, 800, "OpenGL Sphere Latitudes", NULL, NULL);
    if (window == NULL) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    Shader shader("./shaders/vertex.vs", "./shaders/fragment.fs");
    unsigned int shaderProgram = shader.ID;
    unsigned int VAO = 0;
    unsigned int VBO = 0;
    int numSegments = 20; // 每条纬线的段数
    int numLatitudes = 10; // 纬线的数量
    float radius = 0.5f;   // 球体的半径
    createVBOVAO(VAO, VBO, numSegments, numLatitudes, radius);

    while(!glfwWindowShouldClose(window)) {
        processInput(window);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);

        glUseProgram(shaderProgram);
        glBindVertexArray(VAO);

        // 绘制每一条纬线
        for (int i = 0; i < numLatitudes; ++i) {
            glDrawArrays(GL_LINE_LOOP, i * numSegments, numSegments);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glfwTerminate();

    return 0;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}
