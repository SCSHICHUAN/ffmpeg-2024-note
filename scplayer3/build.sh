#!/bin/bash

clang -g -o run scplayer3.c `pkg-config --libs --cflags libavutil libavformat libavcodec libswresample libavfilter sdl2`
