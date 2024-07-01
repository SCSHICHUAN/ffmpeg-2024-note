#!/bin/bash

clang -g -o run ffplay.c src/*.c -I./include `pkg-config --libs --cflags libavutil libavformat libavcodec libswresample libavfilter libavdevice libswscale sdl2`
