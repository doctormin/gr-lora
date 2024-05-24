#!/bin/sh

VERSION=${1:-latest}
docker build -t yimin/gr-lora:$VERSION .
