#!/bin/bash

clang -g -o r2un player.c `pkg-config --libs --cflags libavutil libavformat libavcodec libswresample libavfilter sdl2`
