#!/bin/bash
set -e

echo "Compilation du deamon"
mkdir -p "sys/bin"
mkdir -p "share"
mkdir -p "share/config/"
mkdir -p "share/run/"
mkdir -p "share/logs/"
mkdir -p "share/logs/omsy/"
mkdir -p "share/config/omsy"

gcc src/omsyd.c include/common.c -o sys/bin/omsyd

cat > "share/config/omsy/omsy.conf" <<EOF
path="/Users/julie/Documents/Omsy Web Control"
log="/Users/julie/Documents/Omsy Web Control/share/logs/omsy"
run="/Users/julie/Documents/Omsy Web Control/share/run"
conf="/Users/julie/Documents/OmsyServer/share/config/omsyctl"
EOF