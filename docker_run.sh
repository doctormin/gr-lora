#!/bin/sh
# For macOS (arm64) users, you need to install XQuartz and start it
# enable SSH X11 forwarding inside container (https://stackoverflow.com/q/48235040)
touch /tmp/.docker.xauth
docker run -i -d --privileged \
     -e DISPLAY=host.docker.internal:0 \
     -v /tmp/.docker.xauth:/tmp/.docker.xauth \
     -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
     --volume="$HOME/.Xauthority:/root/.Xauthority:rw" \
     --net=host \
     --entrypoint /bin/zsh \
     yimin/gr-lora 
