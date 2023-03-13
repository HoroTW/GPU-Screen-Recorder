#!/bin/sh -e

## If you are not using systemd:
killall -SIGUSR1 gpu-screen-recorder
## Otherwise:
#systemctl --user kill -s SIGUSR1 gpu-screen-recorder.service

notify-send -t 5000 -u low -- "GPU Screen Recorder" "Replay saved"
