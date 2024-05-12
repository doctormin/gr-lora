#!/bin/sh
# For macOS (arm64) users, you need to install XQuartz and start it
DOCKER_XAUTH=/tmp/.docker.xauth
touch /tmp/.docker.xauth
DISPLAY=host.docker.internal:0
docker run -i -t --rm --privileged -e DISPLAY=$DISPLAY -v /tmp/.docker.xauth:/tmp/.docker.xauth:ro -v /tmp/.X11-unix:/tmp/.X11-unix:ro --volume="$HOME/.Xauthority:/root/.Xauthority:rw" --net=host -v /dev/bus/usb:/dev/bus/usb --entrypoint /bin/bash jkadbear/gr-lora:latest 
