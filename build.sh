#!/bin/bash
#Little script to build qemu the way I'm using it.
#Assumes it's run in a sb2 env

#I'm disabling SDL for now since it doesn't work on the pre as one would hope:
#--assumes hw_surface, but that should just mean this is slower than it needs to be
#--assumes resolution of 640x480, and a bunch of the code acts on this.  might be fixable
#--enable-sdl \
#--audio-drv-list=sdl \

#NOTE(shivaram): I don't find a need for x86_64 or user mode emulation in day to day work
#Disabling them for faster builds. Enable them if required
#--target-list=arm-linux-user,i386-linux-user,x86_64-linux-user,arm-softmmu,i386-softmmu,x86_64-softmmu \

./configure \
--prefix= \
--disable-sdl \
--disable-kvm \
--disable-system \
--enable-curses \
--audio-drv-list= \
--target-list=arm-softmmu,i386-softmmu \
--extra-cflags="-I/usr/local/include -I/usr/local/include/ncurses" \
--extra-ldflags="-Wl,-rpath=/usr/local/lib -L/usr/local/lib" \
&&

make -j8 && \

make DESTDIR=`pwd`/../install install


