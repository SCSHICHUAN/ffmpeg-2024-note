#!/bin/bash

clang -g -o run scplayer1.c `pkg-config --libs --cflags libavutil libavformat libavcodec sdl2`
