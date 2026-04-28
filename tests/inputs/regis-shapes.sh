#!/bin/sh
# Wrap the ReGIS commands in a DCS envelope (ESC P 1 p ... ESC \).
# mlterm's parser intercepts the DCS and routes the body through
# the ReGIS interpreter when the prefix is "1p".
printf '\033P1p'
cat "$(dirname "$0")/regis-shapes.regis"
printf '\033\\'
