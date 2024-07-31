/* 
  fragment.strings
  opengl环境

  Created by Stan on 2022/12/13.
  
*/
#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D texture1;
uniform sampler2D texture2;

void main()
{
    //mix 把两个纹理混合 一个0.4 ，另一个 （1-0.40）
    FragColor = mix(texture(texture1, TexCoord), texture(texture2, TexCoord), 0.5);
}
