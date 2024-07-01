#!/bin/bash

clang -g -o run player.c `pkg-config --libs --cflags libavutil libavformat libavcodec libswresample libavfilter sdl2`
