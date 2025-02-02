# Automatically generated by configure - do not modify!
shared=
build_suffix=
prefix=/Users/stan/Desktop/ffmpeg-2024-note/FFMPEG_5.0/thin/arm64
libdir=${prefix}/lib
incdir=${prefix}/include
rpath=
source_path=src
LIBPREF=lib
LIBSUF=.a
extralibs_avutil="-pthread -lm -framework VideoToolbox -framework CoreFoundation -framework CoreMedia -framework CoreVideo"
extralibs_avcodec="-liconv -lm -framework AudioToolbox -pthread -lz -framework VideoToolbox -framework CoreFoundation -framework CoreMedia -framework CoreVideo"
extralibs_avformat="-lm -lbz2 -lz -Wl,-framework,CoreFoundation -Wl,-framework,Security"
extralibs_avdevice="-framework Foundation -framework AudioToolbox -framework CoreAudio -lm -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework AudioToolbox -pthread"
extralibs_avfilter="-pthread -lm -framework Metal -framework VideoToolbox -framework CoreFoundation -framework CoreMedia -framework CoreVideo"
extralibs_postproc="-lm"
extralibs_swscale="-lm"
extralibs_swresample="-lm"
avdevice_deps="avfilter swscale avformat avcodec swresample avutil"
avfilter_deps="swscale avformat avcodec swresample avutil"
swscale_deps="avutil"
postproc_deps="avutil"
avformat_deps="avcodec swresample avutil"
avcodec_deps="swresample avutil"
swresample_deps="avutil"
avutil_deps=""
