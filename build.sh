#!/usr/bin/env sh

CONTAINER_NAME="builder-$(cat /dev/urandom | head -c 128 | sha1sum | head -c 12)"
docker create --name="$CONTAINER_NAME" muslcc/x86_64:arm-linux-musleabi gcc -static -Os -o /root/loopboot /root/loopboot.c

docker cp loopboot.c "$CONTAINER_NAME:/root/loopboot.c"
docker start -i "$CONTAINER_NAME"
docker cp "$CONTAINER_NAME:/root/loopboot" .
docker rm "$CONTAINER_NAME"
