#!/bin/bash
set -e

echo "Desktop.interface[0] Preparation du deamon"
mkdir -p "sys/bin"
mkdir -p "share"
mkdir -p "share/config/"
mkdir -p "share/run/"
mkdir -p "share/logs/"
mkdir -p "share/logs/omsy/"
mkdir -p "share/config/omsy"

echo "Desktop.interface[1] Compilation de Omsy Deamon"
python3 -m pip install --no-binary :all: --upgrade --target sys/lib psutil
gcc src/omsyd.c include/common.c -o sys/bin/omsyd

echo "Configuration de Omsy Deamon"
cat > "share/config/omsy/omsy.conf" <<EOF
path="/Users/julie/Documents/Omsy Web Control"
log="/Users/julie/Documents/Omsy Web Control/share/logs/omsy"
run="/Users/julie/Documents/Omsy Web Control/share/run"
conf="/Users/julie/Documents/OmsyServer/share/config/omsyctl"
EOF

echo "Desktop.interface[2] Démarrage de Omsy Deamon"
./sys/bin/omsyd -c "/Users/julie/Documents/Omsy Web Control/share/config/omsy/omsy.conf"

echo "======================================="
echo "= Stop ce deamon './sys/bin/omsyd'"
echo "= Start ce deamon ./sys/bin/omsyd -c \"/Users/julie/Documents/Omsy Web Control/share/config/omsy/omsy.conf\""
echo "======================================="
echo "Powered by Floagg EI"
echo "======================================="

echo "Desktop.interface[ENDED]"