#!/bin/bash

clang -g -o run scplayer2.c `pkg-config --libs --cflags libavutil libavformat libavcodec libswresample sdl2`
