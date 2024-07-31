//
//  shader.h
//  opengl环境
//
//  Created by Stan on 2022/12/13.
//

#ifndef shader_h
#define shader_h

#include <glad/glad.h>

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace std;
class Shader
{
public:
    unsigned int ID;
    Shader(const char* vertexPath,const char* fragmentPath){
        string vertexCode;
        string fragmentCode;
        ifstream vShaderFile;
        ifstream fShaderFile;
        //badbit表示发生系统级的错误,failbit 表示发生可恢复的错误
        vShaderFile.exceptions(ifstream::failbit | ifstream::badbit);
        fShaderFile.exceptions(ifstream::failbit | ifstream::badbit);
        
        try {
            //path->file
            vShaderFile.open(vertexPath);
            fShaderFile.open(fragmentPath);
            std::stringstream vShaderStream,fShaderStream;
            //file->stream
            vShaderStream << vShaderFile.rdbuf();
            fShaderStream << fShaderFile.rdbuf();
            vShaderFile.close();
            fShaderFile.close();
            //stream->str
            vertexCode = vShaderStream.str();
            fragmentCode = fShaderStream.str();
            
        } catch (std::ifstream::failure &E) {
            cout << "ERROR: load shader file:" << E.what() << endl;
            return;
        }
        
        const char* vShaderCode = vertexCode.c_str();
        const char* fShaderCode = fragmentCode.c_str();
        
        unsigned int vertex,fragment;
        //vertex shader
        vertex = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex,1,&vShaderCode,NULL);
        glCompileShader(vertex);
        checkCompileErrors(vertex, "VERTEX");
        //fragment shader
        fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment,1,&fShaderCode,NULL);
        glCompileShader(fragment);
        checkCompileErrors(fragment, "FRAGMENT");
        //shader program
        ID = glCreateProgram();
        glAttachShader(ID,vertex);
        glAttachShader(ID,fragment);
        glLinkProgram(ID);
        checkCompileErrors(ID, "PROGRAM");
        //delete
        glDeleteShader(vertex);
        glDeleteShader(fragment);
    }
    //activate program
    void use(unsigned int ID){
        glUseProgram(ID);
    }
    //utility uniform functions ,const 函数不能修改成员变量
    void setBool(const string &name,bool value) const {
        glUniform1i(glGetUniformLocation(ID,name.c_str()),int(value));
    }
    void setInt(const string &name,int value) const {
        glUniform1i(glGetUniformLocation(ID,name.c_str()),value);
    }
    void setFloat(const string &name,float value) const {
        glUniform1f(glGetUniformLocation(ID,name.c_str()),value);
    }
    
    
    
    
private:
    void checkCompileErrors(unsigned int shader,string type){
        int success;
        char infoLogs[1024];
        
        if(type != "PROGRAM"){
            glGetShaderiv(shader,GL_COMPILE_STATUS,&success);
            if(!success){
                //shader compile failed
                glGetShaderInfoLog(shader,1024,NULL,infoLogs);
                cout << "ERROR: shader compile error type:" << type << "\n" << infoLogs << "\n-------" << endl;
            }
        }else{
            glGetProgramiv(shader,GL_LINK_STATUS,&success);
            if(!success){
                //shader link failed
                glGetProgramInfoLog(shader,1024,NULL,infoLogs);
                cout << "ERROR: shader link error of type:" << type << "\n" << infoLogs << "\n-----" << endl;
            }
        }
        
    }
};
#endif /* shader_h */
